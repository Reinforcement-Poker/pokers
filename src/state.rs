#![allow(unused)]
#[cfg(test)]
use proptest_derive::Arbitrary;
use pyo3::prelude::*;
pub mod action;
pub mod card;
pub mod stage;
use action::{ActionEnum, ActionRecord};
use card::Card;
use stage::Stage;
use std::collections::HashSet;
use crate::game_logic::StateMachine;
use std::collections::HashMap;

/// Context for a single betting round
#[pyclass]
#[derive(Debug, Clone)]
pub struct BettingRoundContext {
    #[pyo3(get, set)]
    pub amount_to_call: f64,
    #[pyo3(get, set)]
    pub last_raiser_idx: Option<u64>,
    #[pyo3(get, set)]
    pub last_raise_amount: f64, // Track the size of the last raise increment
    #[pyo3(get, set)]
    pub actions_this_round: usize,
    #[pyo3(get, set)]
    pub players_in_round: usize,
    #[pyo3(get, set)]
    pub starting_player: u64,
    #[pyo3(get, set)]
    pub player_acted: HashSet<u64>, // Track players who have acted
    #[pyo3(get, set)]
    pub player_action_counts: HashMap<u64, usize>, // Per-player action count for the current street
}

#[pymethods]
impl BettingRoundContext {
    #[new]
    pub fn new() -> Self {
        BettingRoundContext::default()
    }
}

#[pyclass]
#[derive(Debug)]
pub struct State {
    #[pyo3(get, set)]
    pub current_player: u64,

    #[pyo3(get, set)]
    pub players_state: Vec<PlayerState>,

    #[pyo3(get, set)]
    pub public_cards: Vec<Card>,

    #[pyo3(get, set)]
    pub stage: Stage,

    #[pyo3(get, set)]
    pub button: u64,

    #[pyo3(get, set)]
    pub from_action: Option<ActionRecord>,

    #[pyo3(get, set)]
    pub action_list: Vec<ActionRecord>,

    #[pyo3(get, set)]
    pub legal_actions: Vec<ActionEnum>,

    // Detailed legal actions for the current player, including BetRaise with concrete sizing
    #[pyo3(get, set)]
    pub legal_actions_detailed: Vec<action::Action>,

    #[pyo3(get, set)]
    pub deck: Vec<Card>,

    #[pyo3(get, set)]
    pub pot: f64,

    #[pyo3(get, set)]
    pub min_bet: f64,

    #[pyo3(get, set)]
    pub sb: f64,

    #[pyo3(get, set)]
    pub bb: f64,

    // Allowed bet/raise sizings expressed as pot multipliers (e.g., 1.0, 2.0, 3.0)
    #[pyo3(get, set)]
    pub betraise_multipliers: Vec<f64>,

    #[pyo3(get, set)]
    pub final_state: bool,

    #[pyo3(get, set)]
    pub status: StateStatus,

    #[pyo3(get, set)]
    pub verbose: bool,

    #[pyo3(get, set)]
    pub seed: u64,

    #[pyo3(get, set)]
    pub context: BettingRoundContext,

    // This field is for internal Rust logic, not exposed to Python
    pub fsm: StateMachine,
}

impl Clone for State {
    fn clone(&self) -> Self {
        State {
            current_player: self.current_player,
            players_state: self.players_state.clone(),
            public_cards: self.public_cards.clone(),
            stage: self.stage,
            button: self.button,
            from_action: self.from_action.clone(),
            action_list: self.action_list.clone(),
            legal_actions: self.legal_actions.clone(),
            legal_actions_detailed: self.legal_actions_detailed.clone(),
            deck: self.deck.clone(),
            pot: self.pot,
            min_bet: self.min_bet,
            sb: self.sb,
            bb: self.bb,
            betraise_multipliers: self.betraise_multipliers.clone(),
            final_state: self.final_state,
            status: self.status,
            verbose: self.verbose,
            seed: self.seed,
            context: self.context.clone(),
            fsm: self.fsm.clone(),
        }
    }
}

impl Default for BettingRoundContext {
    fn default() -> Self {
        BettingRoundContext {
            amount_to_call: 0.0,
            last_raiser_idx: None,
            last_raise_amount: 0.0,
            actions_this_round: 0,
            players_in_round: 0,
            starting_player: 0,
            player_acted: HashSet::new(),
            player_action_counts: HashMap::new(),
        }
    }
}

#[pyclass]
#[derive(Debug, Clone, Copy)]
pub struct PlayerState {
    #[pyo3(get, set)]
    pub player: u64,

    #[pyo3(get, set)]
    pub hand: (Card, Card),

    #[pyo3(get, set)]
    pub bet_chips: f64,

    #[pyo3(get, set)]
    pub pot_chips: f64,

    #[pyo3(get, set)]
    pub stake: f64,

    #[pyo3(get, set)]
    pub reward: f64,

    #[pyo3(get, set)]
    pub active: bool,

    #[pyo3(get, set)]
    pub range_idx: i64,

    #[pyo3(get, set)]
    pub last_stage_action: Option<ActionEnum>,
}

#[pymethods]
impl PlayerState {
    pub fn __str__(&self) -> PyResult<String> {
        Ok(format!("{:#?}", self))
    }
}

#[pyclass]
#[derive(Debug, Clone, Copy)]
pub enum StateStatus {
    Ok,
    IllegalAction,
    HighBet,
}

impl State {
    /// Hand ranking lookup table - maps card combination to rank (1-169)
    /// Based on the C++ evaluate_2cards function
    const HAND_RANKINGS: &'static [(i32, i32, i32, i32)] = &[
        // Pairs (high_rank == low_rank, suited ignored)
        (12, 12, 0, 1), // AA
        (11, 11, 0, 2), // KK
        (10, 10, 0, 3), // QQ
        (9, 9, 0, 5),   // JJ
        (8, 8, 0, 10),  // TT
        (7, 7, 0, 17),  // 99
        (6, 6, 0, 21),  // 88
        (5, 5, 0, 29),  // 77
        (4, 4, 0, 36),  // 66
        (3, 3, 0, 46),  // 55
        (2, 2, 0, 50),  // 44
        (0, 0, 0, 51),  // 22
        (1, 1, 0, 52),  // 33
        // Suited hands (high_rank > low_rank, suited=1)
        (12, 11, 1, 4), // AKs
        (12, 10, 1, 6), // AQs
        (11, 10, 1, 7), // KQs
        (12, 9, 1, 8),  // AJs
        (11, 9, 1, 9),  // KJs
        (12, 8, 1, 12), // ATs
        (10, 9, 1, 13), // QJs
        (11, 8, 1, 14), // KTs
        (10, 8, 1, 15), // QTs
        (9, 8, 1, 16),  // JTs
        (12, 7, 1, 19), // A9s
        (11, 7, 1, 22), // K9s
        (8, 7, 1, 23),  // T9s
        (12, 6, 1, 24), // A8s
        (10, 7, 1, 25), // Q9s
        (9, 7, 1, 26),  // J9s
        (12, 3, 1, 28), // A5s
        (12, 5, 1, 30), // A7s
        (11, 6, 1, 37), // K8s
        (8, 6, 1, 38),  // T8s
        (12, 0, 1, 39), // A2s
        (7, 6, 1, 40),  // 98s
        (9, 6, 1, 41),  // J8s
        (10, 6, 1, 43), // Q8s
        (11, 5, 1, 44), // K7s
        (6, 5, 1, 48),  // 87s
        (11, 4, 1, 53), // K6s
        (7, 5, 1, 54),  // 97s
        (11, 3, 1, 55), // K5s
        (5, 4, 1, 56),  // 76s
        (8, 5, 1, 57),  // T7s
        (11, 2, 1, 58), // K4s
        (11, 1, 1, 59), // K3s
        (11, 0, 1, 60), // K2s
        (10, 5, 1, 61), // Q7s
        (6, 4, 1, 62),  // 86s
        (4, 3, 1, 63),  // 65s
        (9, 5, 1, 64),  // J7s
        (3, 2, 1, 65),  // 54s
        (10, 4, 1, 66), // Q6s
        (5, 3, 1, 67),  // 75s
        (7, 4, 1, 68),  // 96s
        (10, 3, 1, 69), // Q5s
        (4, 2, 1, 70),  // 64s
        (10, 2, 1, 71), // Q4s
        (10, 1, 1, 72), // Q3s
        (8, 4, 1, 74),  // T6s
        (10, 0, 1, 75), // Q2s
        (3, 1, 1, 77),  // 53s
        (6, 3, 1, 78),  // 85s
        (9, 4, 1, 79),  // J6s
        (9, 3, 1, 82),  // J5s
        (2, 1, 1, 84),  // 43s
        (5, 2, 1, 85),  // 74s
        (9, 2, 1, 86),  // J4s
        (9, 1, 1, 87),  // J3s
        (7, 3, 1, 88),  // 95s
        (9, 0, 1, 89),  // J2s
        (4, 1, 1, 90),  // 63s
        (3, 0, 1, 92),  // 52s
        (8, 3, 1, 93),  // T5s
        (6, 2, 1, 94),  // 84s
        (8, 2, 1, 95),  // T4s
        (8, 1, 1, 96),  // T3s
        (2, 0, 1, 97),  // 42s
        (8, 0, 1, 98),  // T2s
        (5, 1, 1, 103), // 73s
        (1, 0, 1, 105), // 32s
        (7, 2, 1, 106), // 94s
        (7, 1, 1, 107), // 93s
        (4, 0, 1, 110), // 62s
        (7, 0, 1, 111), // 92s
        (6, 1, 1, 116), // 83s
        (6, 0, 1, 118), // 82s
        (5, 0, 1, 120), // 72s
        // Offsuit hands (high_rank > low_rank, suited=0)
        (12, 11, 0, 11), // AKo
        (12, 10, 0, 18), // AQo
        (11, 10, 0, 20), // KQo
        (12, 9, 0, 27),  // AJo
        (11, 9, 0, 31),  // KJo
        (10, 9, 0, 35),  // QJo
        (12, 8, 0, 42),  // ATo
        (11, 8, 0, 45),  // KTo
        (9, 8, 0, 47),   // JTo
        (10, 8, 0, 49),  // QTo
        (8, 7, 0, 73),   // T9o
        (12, 7, 0, 76),  // A9o
        (9, 7, 0, 80),   // J9o
        (11, 7, 0, 81),  // K9o
        (10, 7, 0, 83),  // Q9o
        (12, 6, 0, 91),  // A8o
        (7, 6, 0, 99),   // 98o
        (8, 6, 0, 100),  // T8o
        (12, 3, 0, 101), // A5o
        (12, 5, 0, 102), // A7o
        (12, 2, 0, 104), // A4o
        (9, 6, 0, 108),  // J8o
        (12, 1, 0, 109), // A3o
        (11, 6, 0, 112), // K8o
        (12, 4, 0, 113), // A6o
        (6, 5, 0, 114),  // 87o
        (10, 6, 0, 115), // Q8o
        (12, 0, 0, 117), // A2o
        (7, 5, 0, 119),  // 97o
        (5, 4, 0, 121),  // 76o
        (11, 5, 0, 122), // K7o
        (4, 3, 0, 123),  // 65o
        (8, 5, 0, 124),  // T7o
        (11, 4, 0, 125), // K6o
        (6, 4, 0, 126),  // 86o
        (3, 2, 0, 127),  // 54o
        (11, 3, 0, 128), // K5o
        (9, 5, 0, 129),  // J7o
        (5, 3, 0, 130),  // 75o
        (10, 5, 0, 131), // Q7o
        (11, 2, 0, 132), // K4o
        (11, 1, 0, 133), // K3o
        (7, 4, 0, 134),  // 96o
        (11, 0, 0, 135), // K2o
        (4, 2, 0, 136),  // 64o
        (10, 4, 0, 137), // Q6o
        (3, 1, 0, 138),  // 53o
        (6, 3, 0, 139),  // 85o
        (8, 4, 0, 140),  // T6o
        (10, 3, 0, 141), // Q5o
        (2, 1, 0, 142),  // 43o
        (10, 2, 0, 143), // Q4o
        (10, 1, 0, 144), // Q3o
        (5, 2, 0, 145),  // 74o
        (10, 0, 0, 146), // Q2o
        (9, 4, 0, 147),  // J6o
        (4, 1, 0, 148),  // 63o
        (9, 3, 0, 149),  // J5o
        (7, 3, 0, 150),  // 95o
        (3, 0, 0, 151),  // 52o
        (9, 2, 0, 152),  // J4o
        (9, 1, 0, 153),  // J3o
        (2, 0, 0, 154),  // 42o
        (9, 0, 0, 155),  // J2o
        (6, 2, 0, 156),  // 84o
        (8, 3, 0, 157),  // T5o
        (8, 2, 0, 158),  // T4o
        (1, 0, 0, 159),  // 32o
        (8, 1, 0, 160),  // T3o
        (5, 1, 0, 161),  // 73o
        (8, 0, 0, 162),  // T2o
        (4, 0, 0, 163),  // 62o
        (7, 2, 0, 164),  // 94o
        (7, 1, 0, 165),  // 93o
        (7, 0, 0, 166),  // 92o
        (6, 1, 0, 167),  // 83o
        (6, 0, 0, 168),  // 82o
        (5, 0, 0, 169),  // 72o
    ];

    /// Evaluate 2 cards (hole cards in Texas Hold'em)
    /// Returns ranking based on winning probability: 1 (strongest AA) to 169 (weakest 72o)
    /// Higher rank number = weaker hand
    fn evaluate_2cards(&self, card1: Card, card2: Card) -> i32 {
        // Convert CardRank to 0-12 (where 0=2, 12=A)
        let rank_a = card1.rank as i32;
        let rank_b = card2.rank as i32;
        let suit_a = card1.suit as i32;
        let suit_b = card2.suit as i32;

        // Ensure high_rank >= low_rank for table lookup
        let high_rank = rank_a.max(rank_b);
        let low_rank = rank_a.min(rank_b);

        // Check if suited
        let is_suited = if suit_a == suit_b { 1 } else { 0 };

        // Search through the hand rankings table
        for &(hr, lr, suited, rank) in Self::HAND_RANKINGS {
            if hr == high_rank && lr == low_rank && (high_rank == low_rank || suited == is_suited) {
                return rank;
            }
        }

        // Fallback - should never reach here with valid input
        169
    }

    /// Calculate range index for a player based on their hole cards
    /// Returns 0-168 for preflop (169 hand types), 0-1325 for postflop (1326 combinations)
    pub fn calculate_range_idx(&self, player_id: usize) -> i64 {
        if player_id >= self.players_state.len() {
            return -1;
        }

        let player = &self.players_state[player_id];
        let hand = player.hand;

        // Check if we're in preflop (no public cards)
        if self.public_cards.is_empty() || self.stage == Stage::Preflop {
            // Preflop: use evaluate_2cards function to get proper 1-169 ranking
            let hand_rank = self.evaluate_2cards(hand.0, hand.1);
            // Convert from 1-169 to 0-168 index
            return (hand_rank - 1) as i64;
        } else {
            // Postflop: use canonical suit mapping approach
            // Get canonical suit mapping from community cards
            let canonical_suit_map = self.get_canonical_suit_map();

            let rank1 = hand.0.rank as i64;
            let suit1 = canonical_suit_map[hand.0.suit as usize];
            let canon_card1_idx = rank1 * 4 + suit1;

            let rank2 = hand.1.rank as i64;
            let suit2 = canonical_suit_map[hand.1.suit as usize];
            let canon_card2_idx = rank2 * 4 + suit2;

            // Create unique index for the pair, always sort them (higher index first)
            let c1 = canon_card1_idx.max(canon_card2_idx);
            let c2 = canon_card1_idx.min(canon_card2_idx);

            // Formula generates unique index from 0 to C(52, 2) - 1 = 1325
            return c1 * (c1 - 1) / 2 + c2;
        }
    }

    /// Get canonical suit mapping from community cards
    /// This implementation analyzes community cards to create a mapping that preserves suit isomorphism
    fn get_canonical_suit_map(&self) -> [i64; 4] {
        #[derive(Debug)]
        struct SuitInfo {
            original_suit: usize,
            count: i32,
            rank_mask: u16, // Bitmask of ranks for this suit on the board
        }

        impl SuitInfo {
            fn new(original_suit: usize) -> Self {
                SuitInfo {
                    original_suit,
                    count: 0,
                    rank_mask: 0,
                }
            }
        }

        const NUM_SUITS: usize = 4;
        let mut suit_infos: Vec<SuitInfo> = (0..NUM_SUITS).map(|i| SuitInfo::new(i)).collect();

        // Count occurrences and build rank masks for each suit on the board
        for card in &self.public_cards {
            let suit_idx = card.suit as usize;
            let rank = card.rank as u16;
            suit_infos[suit_idx].count += 1;
            suit_infos[suit_idx].rank_mask |= 1 << rank;
        }

        // Sort by count (descending), then by rank_mask (descending), then by original_suit (ascending)
        suit_infos.sort_by(|a, b| match b.count.cmp(&a.count) {
            std::cmp::Ordering::Equal => match b.rank_mask.cmp(&a.rank_mask) {
                std::cmp::Ordering::Equal => a.original_suit.cmp(&b.original_suit),
                other => other,
            },
            other => other,
        });

        // Create the canonical mapping
        let mut canonical_suit_map = [0i64; 4];
        for (canonical_idx, suit_info) in suit_infos.iter().enumerate() {
            canonical_suit_map[suit_info.original_suit] = canonical_idx as i64;
        }

        canonical_suit_map
    }

    /// Update range_idx for all players
    pub fn update_range_indices(&mut self) {
        for i in 0..self.players_state.len() {
            let range_idx = self.calculate_range_idx(i);
            self.players_state[i].range_idx = range_idx;
        }
    }

    /// Construct detailed legal actions for the current player using configured bet/raise sizes.
    /// Returns a vector of Action where BetRaise actions carry concrete target total bet as amount (pot multiplier).
    pub fn compute_legal_actions_detailed(&self) -> Vec<action::Action> {
        use action::{Action, ActionEnum};
        if self.final_state || self.stage == Stage::Showdown {
            return vec![];
        }
        let mut out: Vec<Action> = Vec::new();
        let player_state = &self.players_state[self.current_player as usize];
        if player_state.stake == 0.0 {
            return out;
        }
        // Always allow Fold
        out.push(Action::new(ActionEnum::Fold, 0.0));
        // Check/Call
        let max_bet = self
            .players_state
            .iter()
            .filter(|ps| ps.active)
            .map(|ps| ps.bet_chips)
            .fold(0.0f64, f64::max);
        if self.betraise_multipliers.is_empty() {
            // When no preset sizings are configured, expose both Check and Call for API completeness.
            // Legality will be enforced by the state machine when applying actions.
            out.push(Action::new(ActionEnum::Check, 0.0));
            out.push(Action::new(ActionEnum::Call, 0.0));
        } else {
            if (player_state.bet_chips + 1e-9) >= max_bet {
                out.push(Action::new(ActionEnum::Check, 0.0));
            } else {
                out.push(Action::new(ActionEnum::Call, 0.0));
            }
        }
        // Bet/Raise sizes from configured multipliers, only if a valid raise is reachable
        // Mirror C++ gating: player must be able to reach at least (max_bet + min_raise_amount)
        if player_state.stake > 0.0 {
            // Determine minimum raise amount: if already beyond BB, use last raise size; else at least BB
            let min_raise_amount = if max_bet > self.bb {
                self.context.last_raise_amount
            } else {
                self.bb
            };
            let min_valid_bet = max_bet + min_raise_amount;
            let current_player_bet = player_state.bet_chips;
            let can_reach_min_raise = current_player_bet + player_state.stake + 1e-9 >= min_valid_bet;
            if can_reach_min_raise {
                if self.betraise_multipliers.is_empty() {
                    // No preset sizings configured: expose a single generic BetRaise option.
                    // Use 1.0 as a sensible default multiplier; engine will clamp to min-raise/all-in if needed.
                    out.push(Action::new(ActionEnum::BetRaise, 1.0));
                } else {
                    // Only include BetRaise sizes the player can afford (desired_total_bet <= current_bet + stake)
                    let max_affordable_total_bet = current_player_bet + player_state.stake;
                    for &m in &self.betraise_multipliers {
                        // Use effective pot (committed + current bets) to scale sizing
                        let desired_total_bet = self.effective_pot() * m;
                        if desired_total_bet <= max_affordable_total_bet + 1e-9 {
                            // Store the multiplier in amount; game_logic interprets it as pot multiplier
                            out.push(Action::new(ActionEnum::BetRaise, m));
                        }
                    }
                }
            }
        }
        out
    }

    /// Internal helper: compute discrete legal action indices following the convention
    /// 0=Fold, 1=Check (if available), 2=Call (if available),
    /// and 3.. = BetRaise options aligned with betraise_multipliers order.
    pub fn get_legal_action_ints_internal(&self) -> Vec<i64> {
    // Base BetRaise slots on detailed actions for sizing, but derive Check/Call legality from FSM
    let detailed = self.compute_legal_actions_detailed();
        let mut out: Vec<i64> = Vec::new();

        // The mapping policy:
        // - Always include 0 for Fold (detailed[0])
    // - If Check is legal, map it to 1.
    // - If Call is legal as well as Check, map Call to 2.
    //   If only Call is legal (no Check), map Call to 1.
        // - For BetRaise entries, map sequentially starting at 3, in the same order as betraise_multipliers
        //   Note: Only added when detailed has BetRaise entries, which it already gates correctly.

    // Query FSM for true legality of Check/Call
    let legal_enums = self.fsm.get_legal_actions(self);
    let has_check = legal_enums.iter().any(|a| matches!(a, ActionEnum::Check));
    let has_call = legal_enums.iter().any(|a| matches!(a, ActionEnum::Call));

        // Always push Fold (0)
        out.push(0);

    // Handle Check/Call slots strictly by convention indices
    if has_check { out.push(1); }
    if has_call { out.push(2); }

        // BetRaise slots start at 3
        let mut next_idx = 3i64;
        for a in &detailed {
            if matches!(a.action, ActionEnum::BetRaise) {
                out.push(next_idx);
                next_idx += 1;
            }
        }
        out
    }

    /// Effective pot helper for sizing decisions.
    /// Returns the sum of chips already committed to the pot (pot_chips)
    /// plus chips currently bet on the table this street (bet_chips) across all players.
    /// This avoids relying on `self.pot` accounting details and reflects real-time pot size.
    pub fn effective_pot(&self) -> f64 {
        self.players_state
            .iter()
            .map(|ps| ps.pot_chips + ps.bet_chips)
            .sum()
    }
}



