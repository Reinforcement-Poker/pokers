// game_logic.rs - Rewritten using State-Machine-Based Architecture
use itertools::Itertools;
use pyo3::exceptions::PyOSError;
use pyo3::prelude::*;
use rand::{seq::SliceRandom, SeedableRng};
use std::collections::HashSet;

use crate::state::action::{Action, ActionEnum, ActionRecord};
use crate::state::card::{Card, CardRank, CardSuit};
use crate::state::{stage::Stage, BettingRoundContext};
use crate::state::{PlayerState, State, StateStatus};

// Minimum stake threshold for players to be considered able to act
// Players with stakes below this are treated as effectively all-in
const MIN_STAKE_THRESHOLD: f64 = 0.1;

// FFI binding to PokerHandEvaluator library
extern "C" {
    fn evaluate_5cards(a: i32, b: i32, c: i32, d: i32, e: i32) -> i32;
    fn evaluate_7cards(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32) -> i32;
}

// Define a macro for verbose printing controlled by environment variable
macro_rules! verbose_println {
    ($state:expr, $($arg:tt)*) => {
        if $state.verbose {
            println!($($arg)*);
            use std::io::Write;
            let _ = std::io::stdout().flush();
        }
    };
}

#[derive(Debug)]
pub struct InitStateError {
    msg: String,
}

impl std::convert::From<InitStateError> for PyErr {
    fn from(err: InitStateError) -> PyErr {
        PyOSError::new_err(err.msg)
    }
}

/// Pot struct for side pot management
#[derive(Debug, Clone)]
pub struct Pot {
    pub amount: f64,
    pub eligible_players: HashSet<u64>,
}

impl Pot {
    pub fn new() -> Self {
        Pot {
            amount: 0.0,
            eligible_players: HashSet::new(),
        }
    }
}

/// The State trait defining the contract for all game states
pub trait GameStateInterface: GameStateInterfaceClone + Send + Sync {
    fn apply_action(
        self: Box<Self>,
        state: &mut State,
        action: Action,
    ) -> Result<Box<dyn GameStateInterface>, StateStatus>;

    fn get_legal_actions(&self, state: &State) -> Vec<ActionEnum>;
    fn state_name(&self) -> String;
    fn is_final(&self) -> bool;
}

// Helper trait to enable cloning of trait objects
pub trait GameStateInterfaceClone: Send + Sync {
    fn clone_box(&self) -> Box<dyn GameStateInterface>;
}

impl<T> GameStateInterfaceClone for T
where
    T: 'static + GameStateInterface + Clone,
{
    fn clone_box(&self) -> Box<dyn GameStateInterface> {
        Box::new(self.clone())
    }
}

impl Clone for Box<dyn GameStateInterface> {
    fn clone(&self) -> Box<dyn GameStateInterface> {
        self.clone_box()
    }
}

/// State for when we are awaiting a player's action
#[derive(Debug, Clone)]
pub struct AwaitingAction {
    pub player_to_act_idx: u64,
}

impl AwaitingAction {
    pub fn new(player_to_act_idx: u64, _context: BettingRoundContext) -> Self {
        AwaitingAction { player_to_act_idx }
    }

    /// Check if the betting round has concluded
    fn is_round_over(&self, state: &State) -> bool {
        let active_players: Vec<_> = state.players_state.iter().filter(|ps| ps.active).collect();

        if active_players.len() <= 1 {
            return true;
        }

        // All players who are not all-in must have acted since the last raise (tracked by stable player id)
        let all_acted = active_players.iter().all(|p| {
            p.stake < MIN_STAKE_THRESHOLD || state.context.player_acted.contains(&p.player)
        });

        if !all_acted {
            return false;
        }

        // All players who are not all-in must have bet the same amount.
        let max_bet = active_players
            .iter()
            .map(|ps| ps.bet_chips)
            .fold(0.0f64, f64::max);
        let all_bets_equal = active_players
            .iter()
            .all(|p| p.stake < MIN_STAKE_THRESHOLD || (p.bet_chips - max_bet).abs() < 1e-9);

        all_bets_equal
    }

    /// Find the next active player who can act
    fn find_next_active_player(&self, state: &State, current_idx: u64) -> Option<u64> {
        let num_players = state.players_state.len() as u64;
        if num_players == 0 {
            return None;
        }

        let mut next_player_idx = (current_idx + 1) % num_players;
        // Loop at most once around the table to find the *next* player
        for _ in 0..num_players {
            if next_player_idx == current_idx {
                // We've looped all the way around and are back at the current player.
                // This can happen if only the current player is active and has chips.
                // In this case, there is no "next" player.
                break;
            }

            let player_state = &state.players_state[next_player_idx as usize];
            // Only consider players who are active and have meaningful chips to act
            // Exclude players with very small stakes (< 0.1) as they are effectively all-in
            if player_state.active && player_state.stake >= MIN_STAKE_THRESHOLD {
                return Some(next_player_idx);
            }
            next_player_idx = (next_player_idx + 1) % num_players;
        }

        None
    }

    /// Validate that an action is legal
    fn validate_action(&self, state: &State, action: &Action) -> bool {
        let legal_actions = self.get_legal_actions(state);
        legal_actions.contains(&action.action)
    }

    /// Convert illegal action to legal alternative
    fn make_action_legal(&self, state: &State, action: Action) -> Action {
        if self.validate_action(state, &action) {
            return action;
        }

        let legal_actions = self.get_legal_actions(state);

        match action.action {
            ActionEnum::Check => {
                if legal_actions.contains(&ActionEnum::Check) {
                    action
                } else if legal_actions.contains(&ActionEnum::Call) {
                    Action::new(ActionEnum::Call, 0.0)
                } else {
                    Action::new(ActionEnum::Fold, 0.0)
                }
            }
            ActionEnum::Call => {
                if legal_actions.contains(&ActionEnum::Call) {
                    action
                } else if legal_actions.contains(&ActionEnum::Check) {
                    Action::new(ActionEnum::Check, 0.0)
                } else {
                    Action::new(ActionEnum::Fold, 0.0)
                }
            }
            ActionEnum::BetRaise => {
                if legal_actions.contains(&ActionEnum::Check) {
                    Action::new(ActionEnum::Check, 0.0)
                } else if legal_actions.contains(&ActionEnum::Call) {
                    Action::new(ActionEnum::Call, 0.0)
                } else if legal_actions.contains(&ActionEnum::Fold) {
                    Action::new(ActionEnum::Fold, 0.0)
                } else {
                    action
                }
            }
            ActionEnum::Fold => action, // Fold should always be legal
        }
    }
}

impl GameStateInterface for AwaitingAction {
    fn apply_action(
        mut self: Box<Self>,
        state: &mut State,
        action: Action,
    ) -> Result<Box<dyn GameStateInterface>, StateStatus> {
        // Enforce per-player max 6 actions per street by downgrading actions
    const MAX_ACTIONS_PER_STREET: usize = 6;
    // Use stable player id (State.players_state[i].player) instead of table index
    let player_id = state.players_state[self.player_to_act_idx as usize].player;
    let count_entry = state.context.player_action_counts.entry(player_id).or_insert(0);
        let mut incoming_action = action;
        if *count_entry >= MAX_ACTIONS_PER_STREET {
            // If attempted BetRaise beyond limit -> downgrade to Check/Call else Fold
            if incoming_action.action == ActionEnum::BetRaise {
                let legal = self.get_legal_actions(state);
                if legal.contains(&ActionEnum::Check) {
                    incoming_action = Action::new(ActionEnum::Check, 0.0);
                } else if legal.contains(&ActionEnum::Call) {
                    incoming_action = Action::new(ActionEnum::Call, 0.0);
                } else if legal.contains(&ActionEnum::Fold) {
                    incoming_action = Action::new(ActionEnum::Fold, 0.0);
                }
            }
        }
        // Validate the action comes from the correct player
        // Safety check: ensure the current player can actually act
        let current_player_state = &state.players_state[self.player_to_act_idx as usize];
        if !current_player_state.active || current_player_state.stake < MIN_STAKE_THRESHOLD {
            verbose_println!(
                state,
                "DEBUG: Current player {} cannot act (active: {}, stake: {})",
                self.player_to_act_idx,
                current_player_state.active,
                current_player_state.stake
            );
            return Ok(Box::new(RoundOver::new()));
        }

    // Capture pre-action context BEFORE applying any state changes (for logging/records)
    let legal_actions_before = state.get_legal_action_ints_internal();
    let pre_action_player = self.player_to_act_idx;
    let pre_action_stage = state.stage;

    // Make sure action is legal
        let actual_action = self.make_action_legal(state, incoming_action);
        let player_idx = self.player_to_act_idx as usize;
        let mut final_action_for_record = actual_action;

        verbose_println!(
            state,
            "DEBUG: Player {} taking action {:?} with amount {}",
            player_idx,
            actual_action.action,
            actual_action.amount
        );

        // Apply the action's effects
        match actual_action.action {
            ActionEnum::Fold => {
                state.players_state[player_idx].active = false;
                state.players_state[player_idx].pot_chips +=
                    state.players_state[player_idx].bet_chips;
                state.players_state[player_idx].bet_chips = 0.0;
                state.players_state[player_idx].reward =
                    -(state.players_state[player_idx].pot_chips);
            }
            ActionEnum::Check => {
                // No chips move on check; legality ensured in make_action_legal/get_legal_actions
                final_action_for_record = Action::new(ActionEnum::Check, 0.0);
            }
            ActionEnum::Call => {
                let max_bet = state
                    .players_state
                    .iter()
                    .filter(|ps| ps.active)
                    .map(|ps| ps.bet_chips)
                    .fold(0.0f64, f64::max);

                let current_player_bet = state.players_state[player_idx].bet_chips;
                let required_chips = (max_bet - current_player_bet).max(0.0);
                let player_stake = state.players_state[player_idx].stake;

                let actual_chips = if required_chips >= player_stake {
                    // Go all-in if can't fully match. Use >= to handle exact amount.
                    state.players_state[player_idx].stake = 0.0;
                    player_stake
                } else {
                    state.players_state[player_idx].stake -= required_chips;
                    required_chips
                };

                state.players_state[player_idx].bet_chips += actual_chips;
                state.pot += actual_chips;

                final_action_for_record = Action::new(ActionEnum::Call, actual_chips);
            }

            ActionEnum::BetRaise => {
                // actual_action.amount is now a multiplier of the pot size
                let pot_multiplier = actual_action.amount;
                let desired_total_bet = state.effective_pot() * pot_multiplier;
                let current_player_bet = state.players_state[player_idx].bet_chips;
                let player_stake = state.players_state[player_idx].stake;

                // Find current maximum bet among all players
                let max_bet = state
                    .players_state
                    .iter()
                    .filter(|ps| ps.active)
                    .map(|ps| ps.bet_chips)
                    .fold(0.0f64, f64::max);

                // Calculate minimum valid raise amount
                let min_raise_amount = if max_bet > state.bb {
                    // If there was a previous bet/raise, must raise by at least the last raise amount
                    state.context.last_raise_amount
                } else {
                    // First raise preflop: must raise by at least the amount that would make it 2x BB
                    // For example: BB=2, first raise minimum should be to 4 (raise by 2)
                    state.bb
                };

                let min_valid_bet = max_bet + min_raise_amount;

                // Calculate actual bet amount
                let actual_total_bet = if player_stake <= min_raise_amount {
                    // Go all-in if insufficient chips for a minimum raise.
                    // Use <= to handle cases where stake is exactly the min raise amount.
                    current_player_bet + player_stake
                } else if desired_total_bet < min_valid_bet {
                    // If desired bet is less than minimum, use minimum (or all-in if can't afford)
                    if current_player_bet + player_stake >= min_valid_bet {
                        min_valid_bet
                    } else {
                        current_player_bet + player_stake // All-in
                    }
                } else {
                    desired_total_bet
                };

                let additional_chips = (actual_total_bet - current_player_bet).max(0.0);
                let final_additional_chips = additional_chips.min(player_stake);

                state.players_state[player_idx].bet_chips += final_additional_chips;
                state.players_state[player_idx].stake -= final_additional_chips;
                state.pot += final_additional_chips;

                // Update minimum bet and raise tracking if this is a valid raise
                let new_bet_amount = state.players_state[player_idx].bet_chips;
                if new_bet_amount > max_bet {
                    // Calculate the actual raise increment: new bet amount - previous max bet
                    let raise_increment = new_bet_amount - max_bet;
                    state.min_bet = new_bet_amount;
                    state.context.last_raise_amount = raise_increment;
                    state.context.last_raiser_idx = Some(self.player_to_act_idx);
                    state.context.player_acted.clear(); // Clear actors on raise
                    // Track by stable player id
                    state.context.player_acted.insert(state.players_state[player_idx].player); // Re-add raiser
                    state.context.actions_this_round = 0; // Reset action count on raise
                }
                final_action_for_record = Action::new(
                    ActionEnum::BetRaise,
                    state.players_state[player_idx].bet_chips,
                );
            }
        }

        // Record the action
        state.players_state[player_idx].last_stage_action = Some(actual_action.action);
    // Track that player has acted (by stable player id)
    state.context.player_acted.insert(state.players_state[player_idx].player);

        state.context.actions_this_round += 1;
        // Increment per-player action count
        *state.context.player_action_counts.entry(player_id).or_insert(0) += 1;

        let action_record = ActionRecord {
            player: pre_action_player,
            action: final_action_for_record,
            stage: pre_action_stage,
            // Use the pre-action legal actions snapshot
            legal_actions: legal_actions_before,
        };
        state.from_action = Some(action_record.clone());
        state.action_list.push(action_record);

        // DEBUG: Monitor for excessive actions that could indicate infinite loops
        if state.action_list.len() > 49 {
            eprintln!(
                "WARNING: Action list length exceeded 49 (current: {}). Possible infinite loop detected!",
                state.action_list.len()
            );
            eprintln!("DEBUG: Current game state:");
            eprintln!("  Stage: {:?}", state.stage);
            eprintln!("  Current player: {}", state.current_player);
            eprintln!("  Player to act: {}", self.player_to_act_idx);
            eprintln!("  Final state: {}", state.final_state);

            eprintln!("DEBUG: Player states:");
            for (i, ps) in state.players_state.iter().enumerate() {
                let total_chips = ps.stake + ps.bet_chips + ps.pot_chips;
                eprintln!(
                    "    Player {}: active={}, stake={:.2}, bet_chips={:.2}, pot_chips={:.2}, total={:.2}",
                    i, ps.active, ps.stake, ps.bet_chips, ps.pot_chips, total_chips
                );
            }

            eprintln!("DEBUG: Last 10 actions:");
            let start_idx = state.action_list.len().saturating_sub(10);
            for (i, action) in state.action_list[start_idx..].iter().enumerate() {
                eprintln!(
                    "    {}: Player {} {:?} amount={:.2} stage={:?}",
                    start_idx + i,
                    action.player,
                    action.action.action,
                    action.action.amount,
                    action.stage
                );
            }

            eprintln!("DEBUG: Round context:");
            eprintln!("  Actions this round: {}", state.context.actions_this_round);
            eprintln!("  Players acted: {:?}", state.context.player_acted);
            eprintln!("  Amount to call: {:.2}", state.context.amount_to_call);
            eprintln!("  Last raiser: {:?}", state.context.last_raiser_idx);

            // Check round over condition
            let is_round_over = self.is_round_over(state);
            eprintln!("DEBUG: Is round over: {}", is_round_over);

            // Check next player availability
            let next_player = self.find_next_active_player(state, self.player_to_act_idx);
            eprintln!("DEBUG: Next active player: {:?}", next_player);

            // Count active players with sufficient chips
            let active_players_count = state.players_state.iter().filter(|p| p.active).count();
            let players_with_chips = state
                .players_state
                .iter()
                .filter(|p| p.active && p.stake >= MIN_STAKE_THRESHOLD)
                .count();
            eprintln!(
                "DEBUG: Active players: {}, Players with chips (>= {:.1}): {}",
                active_players_count, MIN_STAKE_THRESHOLD, players_with_chips
            );
        }

        // If a player folded and they were the second to last active player, the game is over.
        if actual_action.action == ActionEnum::Fold {
            let active_players_count = state.players_state.iter().filter(|p| p.active).count();
            if active_players_count <= 1 {
                verbose_println!(
                    state,
                    "DEBUG: Game ending due to fold, only one player left."
                );
                // The game ends here. We transition to GameOver, and the main loop will
                // call advance_to_next_stage_or_showdown to find the winner.
                return Ok(Box::new(GameOver));
            }
        }

        // Check if round is over
        if self.is_round_over(state) {
            verbose_println!(state, "DEBUG: Round is over, transitioning to next stage");
            return Ok(Box::new(RoundOver::new()));
        }

        // Find next player
        if let Some(next_player_idx) = self.find_next_active_player(state, self.player_to_act_idx) {
            state.current_player = next_player_idx;
            self.player_to_act_idx = next_player_idx;
            Ok(self)
        } else {
            // No more players can act - either no active players or all are all-in
            verbose_println!(state, "DEBUG: No more players can act, round over");
            Ok(Box::new(RoundOver::new()))
        }
    }

    fn get_legal_actions(&self, state: &State) -> Vec<ActionEnum> {
        if state.final_state || state.stage == Stage::Showdown {
            return vec![];
        }

        // Always use the state's current_player, not our internal player_to_act_idx
        let player_state = &state.players_state[state.current_player as usize];

        // If player is all-in or has insufficient chips to act meaningfully, they cannot act
        if player_state.stake < MIN_STAKE_THRESHOLD {
            return vec![];
        }

        let mut legal_actions = vec![ActionEnum::Fold];

        // Allow Check if no bet to call; otherwise allow Call
        let max_bet = state
            .players_state
            .iter()
            .filter(|ps| ps.active)
            .map(|ps| ps.bet_chips)
            .fold(0.0f64, f64::max);
        if (player_state.bet_chips + 1e-9) >= max_bet {
            legal_actions.push(ActionEnum::Check);
        } else {
            legal_actions.push(ActionEnum::Call);
        }

        // Allow BetRaise only if a valid raise (at least min raise) is possible
        // Mirror C++: need to be able to reach at least (max_bet + min_raise_amount)
        if player_state.stake >= MIN_STAKE_THRESHOLD {
            let current_player_bet = player_state.bet_chips;
            // Determine minimum raise amount: if already beyond BB, use last raise size; else at least BB
            let min_raise_amount = if max_bet > state.bb { state.context.last_raise_amount } else { state.bb };
            let min_valid_bet = max_bet + min_raise_amount;
            // If player's total possible bet (current bet + stake) can reach the minimum valid raise, enable BetRaise
            if current_player_bet + player_state.stake + 1e-9 >= min_valid_bet {
                legal_actions.push(ActionEnum::BetRaise);
            }
        }

        legal_actions
    }

    fn state_name(&self) -> String {
        format!("AwaitingAction(Player {})", self.player_to_act_idx)
    }

    fn is_final(&self) -> bool {
        false
    }
}

/// Terminal state for a completed betting round
#[derive(Debug, Clone)]
pub struct RoundOver;

impl RoundOver {
    pub fn new() -> Self {
        RoundOver
    }
}

impl GameStateInterface for RoundOver {
    fn apply_action(
        self: Box<Self>,
        _state: &mut State,
        _action: Action,
    ) -> Result<Box<dyn GameStateInterface>, StateStatus> {
        // No actions allowed when round is over
        Err(StateStatus::IllegalAction)
    }

    fn get_legal_actions(&self, _state: &State) -> Vec<ActionEnum> {
        vec![] // No legal actions in this state
    }

    fn state_name(&self) -> String {
        "RoundOver".to_string()
    }

    fn is_final(&self) -> bool {
        true
    }
}

/// Game over state
#[derive(Debug, Clone)]
pub struct GameOver;

impl GameStateInterface for GameOver {
    fn apply_action(
        self: Box<Self>,
        _state: &mut State,
        _action: Action,
    ) -> Result<Box<dyn GameStateInterface>, StateStatus> {
        Err(StateStatus::IllegalAction)
    }

    fn get_legal_actions(&self, _state: &State) -> Vec<ActionEnum> {
        vec![]
    }

    fn state_name(&self) -> String {
        "GameOver".to_string()
    }

    fn is_final(&self) -> bool {
        true
    }
}

/// Internal FSM state holder
#[derive(Clone)]
pub struct StateMachine {
    pub current_state: Box<dyn GameStateInterface>,
}

impl std::fmt::Debug for StateMachine {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("StateMachine")
            .field("current_state", &self.current_state.state_name())
            .finish()
    }
}

impl StateMachine {
    pub fn new(initial_state: Box<dyn GameStateInterface>) -> Self {
        StateMachine {
            current_state: initial_state,
        }
    }

    pub fn apply_action(&mut self, state: &mut State, action: Action) -> Result<(), StateStatus> {
        let current_state = std::mem::replace(
            &mut self.current_state,
            Box::new(GameOver) as Box<dyn GameStateInterface>,
        );

        match current_state.apply_action(state, action) {
            Ok(new_state) => {
                self.current_state = new_state;
                Ok(())
            }
            Err(status) => {
                // Create a new state for error recovery since we can't restore the moved state
                // In case of error, we transition to GameOver to prevent further actions
                self.current_state = Box::new(GameOver);
                Err(status)
            }
        }
    }

    pub fn get_legal_actions(&self, state: &State) -> Vec<ActionEnum> {
        self.current_state.get_legal_actions(state)
    }

    pub fn is_final(&self) -> bool {
        self.current_state.is_final()
    }
}

#[pymethods]
impl State {
    #[staticmethod]
    #[pyo3(signature = (n_players, button, sb, bb, stake, seed, verbose=false, betraise_multipliers=None))]
    pub fn from_seed(
        n_players: u64,
        button: u64,
        sb: f64,
        bb: f64,
        stake: f64,
        seed: u64,
        verbose: bool,
        betraise_multipliers: Option<Vec<f64>>,
    ) -> Result<State, InitStateError> {
        let mut rng = rand::rngs::StdRng::seed_from_u64(seed);
        let mut deck: Vec<Card> = Card::collect();
        deck.shuffle(&mut rng);

        State::from_deck(n_players, button, sb, bb, stake, deck, verbose, seed, betraise_multipliers)
    }

    #[staticmethod]
    #[pyo3(signature = (n_players, button, sb, bb, stake, deck, verbose=false, seed=0, betraise_multipliers=None))]
    pub fn from_deck(
        n_players: u64,
        button: u64,
        sb: f64,
        bb: f64,
        stake: f64,
        mut deck: Vec<Card>,
        verbose: bool,
        seed: u64,
        betraise_multipliers: Option<Vec<f64>>,
    ) -> Result<State, InitStateError> {
        // Validation
        if n_players < 2 {
            return Err(InitStateError {
                msg: "The number of players must be 2 or more".to_owned(),
            });
        }

        if button >= n_players {
            return Err(InitStateError {
                msg: "The button must be between the players".to_owned(),
            });
        }

        if deck.len() < 2 * n_players as usize {
            return Err(InitStateError {
                msg: "The number of cards in the deck must be at least 2*n_players".to_owned(),
            });
        }

        if sb <= 0.0 {
            return Err(InitStateError {
                msg: "The small blind must be greater than 0".to_owned(),
            });
        }

        if bb < sb {
            return Err(InitStateError {
                msg: "The small blind must be smaller or equal than the big blind".to_owned(),
            });
        }

        if stake < bb {
            return Err(InitStateError {
                msg: "The stake must be greater or equal than the big blind".to_owned(),
            });
        }

        // Create players
        let mut players_state: Vec<PlayerState> = Vec::new();
        for i in 0..n_players {
            let player = (button + i + 1) % n_players;
            let chips = match i {
                _ if player == (button + 1) % n_players => sb,
                _ if player == (button + 2) % n_players => bb,
                _ => 0.0,
            };

            let p_state = PlayerState {
                player: player,
                hand: (deck.remove(0), deck.remove(0)),
                bet_chips: chips,
                pot_chips: 0.0,
                stake: stake - chips,
                reward: 0.0,
                active: true,
                range_idx: -1,
                last_stage_action: None,
            };
            players_state.push(p_state);
        }

        players_state.sort_by_key(|ps| ps.player);

        // Find first player to act (UTG) - depends on number of players
        let first_player = if n_players == 2 {
            // Heads-up: small blind acts first preflop
            (button + 1) % n_players
        } else {
            // Multi-way: UTG (left of big blind) acts first
            (button + 3) % n_players
        };

        // Create betting round context
        let active_players = players_state.iter().filter(|ps| ps.active).count();
        let context = BettingRoundContext {
            amount_to_call: bb,
            last_raiser_idx: None,
            last_raise_amount: bb, // Preflop, the first "raise" is the BB
            actions_this_round: 0,
            players_in_round: active_players,
            starting_player: first_player,
            player_acted: HashSet::new(),
            player_action_counts: std::collections::HashMap::new(),
        };

        let fsm = StateMachine::new(Box::new(AwaitingAction::new(first_player, context.clone())));

        let mut state = State {
            current_player: first_player,
            players_state: players_state,
            public_cards: Vec::new(),
            stage: Stage::Preflop,
            button: button,
            from_action: None,
            action_list: Vec::new(),
            legal_actions: Vec::new(),
            legal_actions_detailed: Vec::new(),
            deck: deck,
            final_state: false,
            pot: sb + bb,
            min_bet: bb,
            sb: sb,
            bb: bb,
            // If not provided, leave empty to signal generic BetRaise (no preset sizings)
            betraise_multipliers: betraise_multipliers.unwrap_or_else(|| vec![]),
            status: StateStatus::Ok,
            verbose: verbose,
            seed: seed,
            context,
            fsm,
        };

        // Update range indices for all players
        state.update_range_indices();

    // Set legal actions from FSM and build detailed variants using configured bet/raise sizes
    state.legal_actions = state.fsm.get_legal_actions(&state);
    state.legal_actions_detailed = state.compute_legal_actions_detailed();

        Ok(state)
    }

    /// Return a freshly computed list of detailed legal actions as Action objects
    pub fn legal_actions_detailed_now(&self) -> Vec<Action> {
        self.compute_legal_actions_detailed()
    }

    /// Convenience: return detailed legal actions as (name, amount) tuples
    pub fn legal_actions_detailed_tuples(&self) -> Vec<(String, f64)> {
        self.compute_legal_actions_detailed()
            .into_iter()
            .map(|a| {
                let name = match a.action {
                    ActionEnum::Fold => "Fold".to_string(),
                    ActionEnum::Check => "Check".to_string(),
                    ActionEnum::Call => "Call".to_string(),
                    ActionEnum::BetRaise => "BetRaise".to_string(),
                };
                (name, a.amount)
            })
            .collect()
    }

    /// Return the current count of detailed legal actions
    pub fn legal_actions_detailed_len(&self) -> usize {
        self.compute_legal_actions_detailed().len()
    }

    pub fn apply_action(&mut self, py: Python<'_>, action: Action) -> PyResult<()> {
        // Release the Python GIL while performing heavy game-state transitions
        py.allow_threads(|| {
            self.apply_action_rs(action);
        });

        Ok(())
    }

    /// Overload: apply_action using index into current detailed legal action list.
    /// This allows callers to pass an integer corresponding to one of the legal actions in order.
    pub fn apply_action_int(&mut self, py: Python<'_>, action_int: usize) -> PyResult<()> {
        if self.final_state || self.stage == Stage::Showdown {
            // Look like existing behavior: finish showdown if needed
            if !self.final_state {
                // Release GIL while handling showdown
                py.allow_threads(|| {
                    self.handle_showdown();
                });
            }
            return Ok(());
        }
        if action_int >= self.legal_actions_detailed.len() {
            // Out of range -> mark illegal
            self.status = StateStatus::IllegalAction;
            return Ok(());
        }
        py.allow_threads(|| {
            self.apply_action_int_rs(action_int);
        });
        Ok(())
    }

    #[pyo3(name = "clone")]
    pub fn py_clone(&self) -> Self {
        self.clone()
    }

    pub fn __str__(&self) -> PyResult<String> {
        Ok(format!("{:#?}", self))
    }

    pub fn reset_in_place(&mut self, py: Python<'_>, initial_stack: Option<f64>, seed: Option<u64>, verbose: Option<bool>) -> PyResult<()> {
        // Reinitialize current State in-place without allocating a brand new Python object.
        // This will: draw a fresh deck, reshuffle, recreate players with equal stacks, reset context & FSM.
        let mut result_state: Option<State> = None;
        let mut result_err: Option<InitStateError> = None;
        py.allow_threads(|| {
            let n_players = self.players_state.len() as u64;
            if n_players == 0 { return; }
            let total_chips: f64 = self.players_state.iter().map(|p| p.stake + p.bet_chips + p.pot_chips).sum();
            let init_stack = initial_stack.unwrap_or_else(|| if total_chips > 0.0 { total_chips / n_players as f64 } else { self.bb * 100.0 });
            let new_seed = seed.unwrap_or(self.seed.wrapping_add(1));
            // Randomize button again
            use rand::{SeedableRng, Rng};
            let mut rng = rand::rngs::StdRng::seed_from_u64(new_seed ^ 0x9e3779b97f4a7c15);
            let button = rng.gen_range(0..n_players);
            match State::from_seed(
                n_players,
                button,
                self.sb,
                self.bb,
                init_stack,
                new_seed,
                verbose.unwrap_or(self.verbose),
                Some(self.betraise_multipliers.clone()),
            ) {
                Ok(ns) => result_state = Some(ns),
                Err(e) => result_err = Some(e),
            }
        });
        if let Some(e) = result_err { return Err(PyErr::from(e)); }
        if let Some(ns) = result_state { *self = ns; }
        Ok(())
    }

    /// Return the current discrete legal action indices for the acting player.
    pub fn get_legal_action_ints(&self) -> Vec<i64> {
        self.get_legal_action_ints_internal()
    }

    /// Python-exposed helper to get the effective pot (committed + current bets)
    #[pyo3(name = "effective_pot")]
    pub fn py_effective_pot(&self) -> f64 {
        State::effective_pot(self)
    }
}

impl State {
    /// Advance to the next stage or handle showdown
    fn advance_to_next_stage_or_showdown(&mut self) {
        verbose_println!(self, "DEBUG: Advancing from stage {:?}", self.stage);

        // --- Return uncalled bets ---
        // This happens at the end of a betting round, before chips are raked into the main pot.
        let players_with_bets: Vec<_> = self
            .players_state
            .iter()
            .filter(|p| p.bet_chips > 0.0)
            .collect();

        if players_with_bets.len() > 1 {
            let mut bet_amounts: Vec<f64> = players_with_bets.iter().map(|p| p.bet_chips).collect();
            bet_amounts.sort_by(|a, b| b.partial_cmp(a).unwrap()); // Sort descending

            let highest_bet = bet_amounts[0];
            let max_called_bet = bet_amounts[1]; // The second highest bet is the amount that was called

            if highest_bet > max_called_bet {
                // Find the player who made the uncalled bet
                if let Some(player_to_refund) = self
                    .players_state
                    .iter_mut()
                    .find(|p| p.bet_chips == highest_bet)
                {
                    let refund_amount = highest_bet - max_called_bet;
                    verbose_println!(
                        self,
                        "DEBUG: Refunding uncalled bet of {:.2} to player {}",
                        refund_amount,
                        player_to_refund.player
                    );
                    player_to_refund.stake += refund_amount;
                    player_to_refund.bet_chips -= refund_amount; // Adjust their bet down to the called amount
                }
            }
        }

        // Move all bet_chips to pot_chips
        for player_state in &mut self.players_state {
            player_state.pot_chips += player_state.bet_chips;
            player_state.bet_chips = 0.0;
            player_state.last_stage_action = None; // Reset for new stage
        }

        // Clear the players acted tracking for the new round
        self.context.player_acted.clear();

        // Advance stage
        self.stage = match self.stage {
            Stage::Preflop => Stage::Flop,
            Stage::Flop => Stage::Turn,
            Stage::Turn => Stage::River,
            Stage::River => {
                // When we reach showdown, handle it immediately
                self.stage = Stage::Showdown;
                self.handle_showdown();
                return;
            }
            Stage::Showdown => {
                self.handle_showdown();
                return;
            }
        };

        // Deal community cards
        let cards_to_deal = match self.stage {
            Stage::Flop => 3,
            Stage::Turn | Stage::River => 1,
            _ => 0,
        };

        for _ in 0..cards_to_deal {
            if !self.deck.is_empty() {
                self.public_cards.push(self.deck.remove(0));
            }
        }

        verbose_println!(
            self,
            "DEBUG: Advanced to {:?}, dealt {} cards",
            self.stage,
            cards_to_deal
        );

        // Reset min_bet for new round
        self.min_bet = 0.0;

        // Check if we should go straight to showdown
        let active_players: Vec<&PlayerState> =
            self.players_state.iter().filter(|ps| ps.active).collect();

        let players_with_chips = active_players
            .iter()
            .filter(|ps| ps.stake >= MIN_STAKE_THRESHOLD)
            .count();

        if active_players.len() <= 1 || players_with_chips <= 1 {
            verbose_println!(
                self,
                "DEBUG: Not enough players to continue, going to showdown"
            );
            self.complete_to_showdown();
            return;
        }

        // Find first player to act (left of button)
        let first_player = (self.button + 1) % self.players_state.len() as u64;
        self.current_player = first_player;
        let mut attempts = 0;
        let max_attempts = self.players_state.len();

        while attempts < max_attempts {
            let player_state = &self.players_state[self.current_player as usize];
            if player_state.active && player_state.stake >= MIN_STAKE_THRESHOLD {
                break;
            }

            self.current_player = (self.current_player + 1) % self.players_state.len() as u64;
            attempts += 1;
        }

        if attempts >= max_attempts {
            verbose_println!(self, "DEBUG: No players can act, going to showdown");
            self.complete_to_showdown();
            return;
        }

        // Reset context for the new betting round
        let active_players = self.players_state.iter().filter(|ps| ps.active).count();
        self.context = BettingRoundContext {
            amount_to_call: 0.0, // No bet to call at the start of a new round
            last_raiser_idx: None,
            last_raise_amount: self.bb, // Minimum bet/raise is the big blind
            actions_this_round: 0,
            players_in_round: active_players,
            starting_player: self.current_player,
            player_acted: HashSet::new(),
            player_action_counts: std::collections::HashMap::new(),
        };

    self.fsm = StateMachine::new(Box::new(AwaitingAction::new(
            self.current_player,
            self.context.clone(),
        )));
    self.legal_actions = self.fsm.get_legal_actions(self);
    self.legal_actions_detailed = self.compute_legal_actions_detailed();

    }

    /// Complete to showdown and handle final outcome
    fn complete_to_showdown(&mut self) {
        verbose_println!(self, "DEBUG: Completing to showdown");

        // Deal remaining community cards if needed
        match self.stage {
            Stage::Preflop => {
                // Deal flop, turn, river
                for _ in 0..5 {
                    if !self.deck.is_empty() {
                        self.public_cards.push(self.deck.remove(0));
                    }
                }
            }
            Stage::Flop => {
                // Deal turn, river
                for _ in 0..2 {
                    if !self.deck.is_empty() {
                        self.public_cards.push(self.deck.remove(0));
                    }
                }
            }
            Stage::Turn => {
                // Deal river
                if !self.deck.is_empty() {
                    self.public_cards.push(self.deck.remove(0));
                }
            }
            _ => {} // Already have all cards
        }

        self.stage = Stage::Showdown;
        self.handle_showdown();
    }

    /// Handle showdown logic
    fn handle_showdown(&mut self) {
        verbose_println!(self, "DEBUG: Handling showdown");

        let active_players: Vec<PlayerState> = self
            .players_state
            .iter()
            .copied()
            .filter(|ps| ps.active)
            .collect();

        if active_players.len() <= 1 {
            // Only one player left - they win everything
            if let Some(winner) = active_players.first() {
                self.set_winners(vec![winner.player]);
            } else {
                // No active players - should not happen, but handle gracefully
                self.final_state = true;
            }
        } else {
            // Multiple players - evaluate hands
            let mut player_ranks: Vec<(u64, u64)> = active_players
                .iter()
                .map(|ps| {
                    let rank = rank_hand(self, ps.hand, &self.public_cards);
                    (ps.player, rank)
                })
                .collect();

            // Sort by best hand (lowest rank)
            player_ranks.sort_by_key(|&(_, rank)| rank);

            // Find all players with the best hand
            let best_rank = player_ranks[0].1;
            let winners: Vec<u64> = player_ranks
                .iter()
                .filter(|(_, rank)| *rank == best_rank)
                .map(|(player, _)| *player)
                .collect();

            self.set_winners(winners);
        }

        // Ensure the game is marked as final after showdown and update FSM
        self.final_state = true;
        self.fsm = StateMachine::new(Box::new(GameOver));
    self.legal_actions = vec![];
    self.legal_actions_detailed = vec![];
    }

    /// Set winners and calculate rewards
    fn set_winners(&mut self, winners: Vec<u64>) {
        verbose_println!(self, "DEBUG: Setting winners: {:?}", winners);

        // Move all bet_chips to pot_chips for final calculation
        for p in &mut self.players_state {
            p.pot_chips += p.bet_chips;
            p.bet_chips = 0.0;
        }

        // Calculate and distribute rewards using side pot logic
        resolve_pots(self, &winners);

        // Set all players to inactive and mark game as final
        for p in &mut self.players_state {
            p.active = false;
        }

        // Final check to ensure state is marked as final
        if self.final_state == false {
            self.final_state = true;
        }

        self.final_state = true;
    }

    /// Internal heavy apply_action implementation that does not require Python GIL
    pub fn apply_action_rs(&mut self, action: Action) {
        // If we're at showdown, no actions are allowed - handle showdown and finish
        if self.stage == Stage::Showdown {
            if !self.final_state {
                self.handle_showdown();
            }
            self.legal_actions_detailed = vec![];
            return;
        }

        // Temporarily take ownership of the FSM to avoid conflicting mutable borrows of `self`.
        let mut fsm = std::mem::replace(&mut self.fsm, StateMachine::new(Box::new(GameOver)));

        // Now `fsm` is a separate variable, and we can pass `&mut self` to its methods.
        match fsm.apply_action(self, action) {
            Ok(()) => {
                if fsm.is_final() && !self.final_state {
                    // The round has ended. Advance the game stage.
                    // This function will set a new FSM for the new stage, so we don't put the old `fsm` back.
                    self.advance_to_next_stage_or_showdown();
                } else {
                    // The round is not over. Update legal actions for the next player
                    // and put the updated FSM back into the state.
                    if !self.final_state {
                        self.legal_actions = fsm.get_legal_actions(self);
                        self.legal_actions_detailed = self.compute_legal_actions_detailed();
                    } else {
                        // Game is final, no legal actions
                        self.legal_actions = vec![];
                        self.legal_actions_detailed = vec![];
                    }
                    self.fsm = fsm;
                }
            }
            Err(status) => {
                self.status = status;
                // On error, the FSM is now in a terminal state (GameOver).
                // Put it back to prevent further actions.
                self.fsm = fsm;
            }
        }
    }

    /// Internal helper to apply action by integer index without requiring Python GIL
    pub fn apply_action_int_rs(&mut self, action_int: usize) {
        if self.final_state || self.stage == Stage::Showdown {
            if !self.final_state {
                self.handle_showdown();
            }
            return;
        }
        if action_int >= self.legal_actions_detailed.len() {
            // Out of range -> mark illegal
            self.status = StateStatus::IllegalAction;
            return;
        }
        let action = self.legal_actions_detailed[action_int];
        self.apply_action_rs(action);
    }
}

/// Resolve pots and distribute winnings using correct side pot algorithm
pub fn resolve_pots(state: &mut State, _winners: &[u64]) {
    verbose_println!(state, "DEBUG: Starting resolve_pots");

    // CRITICAL FIX: Reset all player rewards to zero before calculating winnings for this hand.
    for p in &mut state.players_state {
        p.reward = 0.0;
    }

    // All players who have pot_chips > 0 contributed to the pot
    let contributing_players: Vec<_> = state
        .players_state
        .iter()
        .filter(|p| p.pot_chips > 0.0)
        .cloned()
        .collect();

    if contributing_players.is_empty() {
        verbose_println!(state, "DEBUG: No contributing players, returning");
        return;
    }

    verbose_println!(state, "DEBUG: Contributing players:");
    for player in &contributing_players {
        verbose_println!(
            state,
            "  Player {}: pot_chips={:.2}, active={}",
            player.player,
            player.pot_chips,
            player.active
        );
    }

    // Get unique investment levels and sort them
    let mut investment_levels: Vec<f64> =
        contributing_players.iter().map(|p| p.pot_chips).collect();
    investment_levels.sort_by(|a, b| a.partial_cmp(b).unwrap());
    investment_levels.dedup_by(|a, b| (*a - *b).abs() < 1e-9);

    verbose_println!(state, "DEBUG: Investment levels: {:?}", investment_levels);

    // Create discrete pots
    let mut pots: Vec<(f64, Vec<u64>)> = Vec::new(); // (pot_amount, eligible_players)
    let mut previous_level = 0.0;

    for &level in &investment_levels {
        let contribution_per_player = level - previous_level;
        if contribution_per_player <= 1e-9 {
            continue;
        }

        // Find players who contributed to this pot level
        let eligible_players: Vec<u64> = contributing_players
            .iter()
            .filter(|p| p.pot_chips >= level)
            .map(|p| p.player)
            .collect();

        if !eligible_players.is_empty() {
            let pot_amount = contribution_per_player * eligible_players.len() as f64;

            verbose_println!(state, "DEBUG: Creating pot - Level: {:.2}, Contribution: {:.2}, Eligible: {:?}, Amount: {:.2}",
                           level, contribution_per_player, eligible_players, pot_amount);

            pots.push((pot_amount, eligible_players));
        }

        previous_level = level;
    }

    verbose_println!(state, "DEBUG: Created {} pots", pots.len());

    // Distribute winnings for each pot, starting from the smallest side pot
    for (pot_amount, eligible_players) in pots.into_iter() {
        if eligible_players.is_empty() {
            continue;
        }

        // Find active players who are eligible for this pot
        let active_eligible: Vec<u64> = eligible_players
            .into_iter()
            .filter(|&player_id| {
                let player_state = &state.players_state[player_id as usize];
                player_state.active // Only active players can win
            })
            .collect();

        if active_eligible.is_empty() {
            // No active players eligible - this shouldn't happen in normal gameplay
            verbose_println!(
                state,
                "DEBUG: No active players eligible for pot of {:.2}",
                pot_amount
            );
            continue;
        }

        verbose_println!(
            state,
            "DEBUG: Processing pot of {:.2}, eligible active players: {:?}",
            pot_amount,
            active_eligible
        );

        // Determine winner(s) by comparing hands among eligible active players
        let mut best_rank = 7463; // Worst possible rank
        let mut pot_winners: Vec<u64> = Vec::new();

        for &player_id in &active_eligible {
            let player_state = &state.players_state[player_id as usize];
            let rank = rank_hand(state, player_state.hand, &state.public_cards);

            if rank < best_rank {
                best_rank = rank;
                pot_winners = vec![player_id];
            } else if rank == best_rank {
                pot_winners.push(player_id);
            }
        }

        verbose_println!(
            state,
            "DEBUG: Pot winners: {:?}, best rank: {:?}",
            pot_winners,
            best_rank
        );

        // Distribute pot among winners
        if !pot_winners.is_empty() {
            let reward_per_winner = pot_amount / pot_winners.len() as f64;

            verbose_println!(
                state,
                "DEBUG: Distributing {:.2} to each of {} winners",
                reward_per_winner,
                pot_winners.len()
            );

            for &winner_id in &pot_winners {
                state.players_state[winner_id as usize].reward += reward_per_winner;
                verbose_println!(
                    state,
                    "DEBUG: Player {} now has reward {:.2}",
                    winner_id,
                    state.players_state[winner_id as usize].reward
                );
            }
        }
    }

    // Calculate final rewards: rewards earned minus amount invested
    verbose_println!(state, "DEBUG: Final reward calculation:");
    let mut total_earned = 0.0;
    let mut total_invested = 0.0;

    for p in &mut state.players_state {
        let earned = p.reward;
        let invested = p.pot_chips;
        total_earned += earned;
        total_invested += invested;

        verbose_println!(
            state,
            "DEBUG: Player {}: earned={:.2}, invested={:.2}",
            p.player,
            earned,
            invested
        );

        p.reward -= p.pot_chips; // Subtract what they put in

        verbose_println!(
            state,
            "DEBUG: Player {} final reward: {:.2}",
            p.player,
            p.reward
        );
    }

    let final_total: f64 = state.players_state.iter().map(|p| p.reward).sum();
    verbose_println!(
        state,
        "DEBUG: Total earned: {:.2}, Total invested: {:.2}, Final sum: {:.6}",
        total_earned,
        total_invested,
        final_total
    );
}

/// Generate legal actions for the current state - fallback function
#[pyfunction]
pub fn legal_actions(state: &State) -> Vec<ActionEnum> {
    if state.final_state || state.stage == Stage::Showdown {
        return vec![];
    }

    let current_player_state = &state.players_state[state.current_player as usize];

    // If player is all-in, they cannot act
    if current_player_state.stake == 0.0 {
        return vec![];
    }

    let mut legal_actions = vec![ActionEnum::Fold];

    // Allow Check if no bet to call; otherwise allow Call
    let max_bet = state
        .players_state
        .iter()
        .map(|ps| ps.bet_chips)
        .fold(0.0f64, f64::max);
    if (current_player_state.bet_chips + 1e-9) >= max_bet {
        legal_actions.push(ActionEnum::Check);
    } else {
        legal_actions.push(ActionEnum::Call);
    }

    // Allow BetRaise if player has chips to bet
    if current_player_state.stake > 0.0 {
        legal_actions.push(ActionEnum::BetRaise);
    }

    legal_actions
}

fn rank_hand(_state: &State, private_cards: (Card, Card), public_cards: &Vec<Card>) -> u64 {
    let mut cards = public_cards.clone();
    cards.append(&mut vec![private_cards.0, private_cards.1]);

    // Check if we have enough cards for a valid combination
    if cards.len() < 5 {
        // Return worst possible rank if not enough cards
        return 7463; // Worse than phevaluator's worst rank (7462)
    }

    // If we have exactly 7 cards, use phevaluator directly
    if cards.len() == 7 {
        let card_ints: Vec<i32> = cards.iter().map(|c| card_to_phevaluator_int(*c)).collect();
        let rank = unsafe {
            evaluate_7cards(
                card_ints[0],
                card_ints[1],
                card_ints[2],
                card_ints[3],
                card_ints[4],
                card_ints[5],
                card_ints[6],
            )
        };

        // Return phevaluator rank directly (1-7462, where 1 is best)
        return rank as u64;
    }

    // For fewer than 7 cards, find the best 5-card combination using phevaluator
    let min_rank = cards
        .iter()
        .copied()
        .combinations(5)
        .map(|comb| {
            if comb.len() == 5 {
                let card_ints: Vec<i32> =
                    comb.iter().map(|c| card_to_phevaluator_int(*c)).collect();
                unsafe {
                    evaluate_5cards(
                        card_ints[0],
                        card_ints[1],
                        card_ints[2],
                        card_ints[3],
                        card_ints[4],
                    ) as u64
                }
            } else {
                7463 // Invalid combination
            }
        })
        .min()
        .unwrap_or(7463);

    min_rank
}

/// Convert a Card to phevaluator integer format
/// phevaluator format: rank * 4 + suit
/// where rank: 2=0, 3=1, ..., A=12
/// and suit: C=0, D=1, H=2, S=3
fn card_to_phevaluator_int(card: Card) -> i32 {
    let rank_value = match card.rank {
        CardRank::R2 => 0,
        CardRank::R3 => 1,
        CardRank::R4 => 2,
        CardRank::R5 => 3,
        CardRank::R6 => 4,
        CardRank::R7 => 5,
        CardRank::R8 => 6,
        CardRank::R9 => 7,
        CardRank::RT => 8,
        CardRank::RJ => 9,
        CardRank::RQ => 10,
        CardRank::RK => 11,
        CardRank::RA => 12,
    };

    let suit_value = match card.suit {
        CardSuit::Clubs => 0,
        CardSuit::Diamonds => 1,
        CardSuit::Hearts => 2,
        CardSuit::Spades => 3,
    };

    rank_value * 4 + suit_value
}

mod tests {
    #[cfg(test)]
    use super::*;
    #[cfg(test)]
    use proptest::prelude::*;

    #[cfg(test)]
    proptest! {
        #[test]
        fn from_deck_doesnt_crash(n_players in 0..10000, deck: Vec<Card>, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions: Vec<Action>) {
            let initial_state = State::from_deck(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, deck, false, 12345, None);
            match initial_state {
                Ok(mut state) => {
                    for action in actions.iter().take(100) {
                        if state.final_state {
                            break;
                        }
                        state.apply_action_rs(*action);
                    }
                }
                Err(_) => {}
            };
        }

        #[test]
        fn zero_sum_game(n_players in 2..26, seed: u64, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions in prop::collection::vec(Action::arbitrary_with(((), ())).prop_filter("Raise abs amount bellow 1e12",
        |a| a.amount.abs() < 1e12), 1..100)) {
            let initial_state = State::from_seed(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, seed, false, None);
            match initial_state {
                Ok(mut state) => {
                    for action in actions {
                        if state.final_state {
                            break;
                        }
                        state.apply_action_rs(action);
                    }
                    let sum: f64 = state.players_state.iter().map(|ps| ps.reward).sum();
                    prop_assert!((sum).abs() < 1e-9);
                }
                Err(_) => {}
            };
        }
    }
}
