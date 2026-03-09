// game_logic.rs
use itertools::Itertools;
use pyo3::exceptions::PyOSError;
use pyo3::prelude::*;
use rand::{seq::SliceRandom, SeedableRng};
use strum::IntoEnumIterator;

use crate::state::action::{Action, ActionEnum, ActionRecord};
use crate::state::card::{Card, CardRank, CardSuit};
use crate::state::stage::Stage;
use crate::state::{PlayerState, State, StateStatus};

// Define a macro for verbose printing
macro_rules! verbose_println {
    ($state:expr, $($arg:tt)*) => {
        if $state.verbose {
            println!($($arg)*);
        }
    };
}

const CHIP_EPSILON: f64 = 1e-9;

pub struct InitStateError {
    msg: String,
}

impl std::convert::From<InitStateError> for PyErr {
    fn from(err: InitStateError) -> PyErr {
        PyOSError::new_err(err.msg)
    }
}

#[pymethods]
impl State {
    #[staticmethod]
    #[pyo3(signature = (n_players, button, sb, bb, stake, seed, verbose=false))]
    pub fn from_seed(
        n_players: u64,
        button: u64,
        sb: f64,
        bb: f64,
        stake: f64,
        seed: u64,
        verbose: bool,
    ) -> Result<State, InitStateError> {
        let mut rng = rand::rngs::StdRng::seed_from_u64(seed);
        let mut deck: Vec<Card> = Card::collect();
        deck.shuffle(&mut rng);

        State::from_deck(n_players, button, sb, bb, stake, deck, verbose)
    }

    #[staticmethod]
    #[pyo3(signature = (n_players, button, sb, bb, stake, deck, verbose=false))]
    pub fn from_deck(
        n_players: u64,
        button: u64,
        sb: f64,
        bb: f64,
        stake: f64,
        mut deck: Vec<Card>,
        verbose: bool,
    ) -> Result<State, InitStateError> {
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

        if sb < 0.0 {
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
                last_stage_action: None,
            };
            players_state.push(p_state);
        }

        players_state.sort_by_key(|ps| ps.player);

        let mut state = State {
            current_player: (button + 3) % n_players,
            players_state: players_state,
            public_cards: Vec::new(),
            stage: Stage::Preflop,
            button: button,
            from_action: None,
            legal_actions: Vec::new(),
            deck: deck,
            final_state: false,
            pot: sb + bb,
            min_bet: bb,
            status: StateStatus::Ok,
            verbose: verbose,
        };

        state.legal_actions = legal_actions(&state);
        Ok(state)
    }

    pub fn apply_action(&self, action: Action) -> State {
        match self.status {
            StateStatus::Ok => (),
            _ => return self.clone(),
        }

        if self.final_state {
            return self.clone();
        }

        let mut new_state = self.clone();
        new_state.from_action = Some(ActionRecord {
            player: self.current_player,
            action: action,
            stage: self.stage,
            legal_actions: self.legal_actions.clone(),
        });

        if !self.legal_actions.contains(&action.action) {
            return State {
                status: StateStatus::IllegalAction,
                final_state: true,
                ..new_state
            };
        }

        let player = self.current_player as usize;

        match action.action {
            ActionEnum::Fold => {
                new_state.players_state[player].active = false;
                new_state.players_state[player].pot_chips += self.players_state[player].bet_chips;
                new_state.players_state[player].bet_chips = 0.0;
                new_state.players_state[player].reward =
                    -(new_state.players_state[player].pot_chips as f64);
            }

            ActionEnum::Call => {
                let raised_chips = self.min_bet - self.players_state[player].bet_chips;
                new_state.players_state[player].bet_chips += raised_chips;
                new_state.players_state[player].stake -= raised_chips;
                new_state.pot += raised_chips;
            }

            ActionEnum::Raise => {
                let bet = (self.min_bet - self.players_state[player].bet_chips) + action.amount;
                if bet > self.players_state[player].stake {
                    return State {
                        status: StateStatus::HighBet,
                        final_state: true,
                        ..new_state
                    };
                }
                new_state.players_state[player].bet_chips += bet;
                new_state.players_state[player].stake -= bet;
                new_state.pot += bet;
                new_state.min_bet = new_state.players_state[player].bet_chips
            }

            ActionEnum::Check => (),
        };

        new_state.players_state[player].last_stage_action = Some(action.action);

        new_state.current_player = (self.current_player + 1) % self.players_state.len() as u64;
        while !self.players_state[new_state.current_player as usize].active {
            new_state.current_player =
                (new_state.current_player + 1) % self.players_state.len() as u64;
        }

        // The betting round ends if:
        // Theres two or more active players
        let active_players: Vec<PlayerState> = new_state
            .players_state
            .iter()
            .copied()
            .filter(|ps| ps.active)
            .collect();
        let multiple_active = active_players.len() >= 2;
        // Every active player has done an action
        let is_last_player = active_players.iter().all(|ps| ps.last_stage_action != None);
        // And every active player has bet the same amount
        let all_same_bet = active_players
            .iter()
            .all(|ps| ps.bet_chips == new_state.min_bet);

        let round_ended = multiple_active && is_last_player && all_same_bet;

        if round_ended {
            new_state.to_next_stage();
        }

        // The game ends if the players have reached the showdown or every player except one has folded
        if active_players.len() == 1 {
            new_state.set_winners(vec![active_players[0].player]);
        }

        if new_state.stage == Stage::Showdown {
            let ranks: Vec<(u64, u64, u64)> = active_players
                .iter()
                .map(|ps| rank_hand(&new_state, ps.hand, &new_state.public_cards))
                .collect();
            let min_rank = ranks.iter().copied().min().unwrap();
            
            // Use verbose_println! macro instead of println!
            verbose_println!(&new_state, "Ranks: {:?}", ranks);
            
            let winners_indices: Vec<usize> = ranks
                .iter()
                .enumerate()
                .filter(|(_, &r)| r == min_rank)
                .map(|(i, _)| i)
                .collect();
                
            // Use verbose_println! macro instead of println!    
            verbose_println!(&new_state, "Winner id: {:?}", winners_indices);
            
            new_state.set_winners(
                winners_indices
                    .iter()
                    .map(|&i| active_players[i].player)
                    .collect(),
            );
        }

        new_state.legal_actions = legal_actions(&new_state);
        new_state
    }

    fn set_winners(&mut self, winners: Vec<u64>) {
        assert!(winners.iter().all(|&p| p < self.players_state.len() as u64));

        let winner_reward = self
            .players_state
            .iter()
            .filter(|&&ps| !winners.contains(&ps.player))
            .map(|ps| ps.pot_chips + ps.bet_chips)
            .fold(0.0, |c1, c2| c1 + c2)
            / winners.len() as f64;

        self.players_state = self
            .players_state
            .iter()
            .map(|ps| PlayerState {
                pot_chips: 0.0,
                bet_chips: 0.0,
                reward: if winners.contains(&ps.player) {
                    winner_reward
                } else {
                    -(ps.pot_chips + ps.bet_chips as f64)
                },
                active: false,
                ..*ps
            })
            .collect();

        self.final_state = true;
    }

    fn to_next_stage(&mut self) {
        self.stage = match self.stage {
            Stage::Preflop => Stage::Flop,
            Stage::Flop => Stage::Turn,
            Stage::Turn => Stage::River,
            _ => Stage::Showdown,
        };
        let n_deal_cards = match self.stage {
            Stage::Flop => 3,
            Stage::Turn | Stage::River => 1,
            _ => 0,
        };
        for _ in 0..n_deal_cards {
            self.public_cards.push(self.deck.remove(0))
        }
        self.players_state = self
            .players_state
            .iter()
            .map(|ps| PlayerState {
                pot_chips: ps.pot_chips + ps.bet_chips,
                bet_chips: 0.0,
                last_stage_action: None,
                ..*ps
            })
            .collect();

        self.min_bet = 0.0;

        self.current_player = (self.button + 1) % self.players_state.len() as u64;
        while !self.players_state[self.current_player as usize].active {
            self.current_player = (self.current_player + 1) % self.players_state.len() as u64;
        }
    }

    pub fn __str__(&self) -> PyResult<String> {
        Ok(format!("{:#?}", self))
    }
}

#[pyfunction]
fn legal_actions(state: &State) -> Vec<ActionEnum> {
    let mut illegal_actions: Vec<ActionEnum> = Vec::new();
    match state.stage {
        Stage::Showdown => illegal_actions.append(&mut ActionEnum::iter().collect()),
        Stage::Preflop => {}
        _ => (),
    }

    if state.final_state {
        illegal_actions.append(&mut ActionEnum::iter().collect());
    }

    if state.min_bet == 0.0 {
        illegal_actions.push(ActionEnum::Call);
    }

    if state.min_bet != 0.0 {
        illegal_actions.push(ActionEnum::Check);
    }

    if !current_player_can_raise(state) {
        illegal_actions.push(ActionEnum::Raise);
    }

    let legal_actions: Vec<ActionEnum> = ActionEnum::iter()
        .filter(|a| !illegal_actions.contains(a))
        .collect();
    legal_actions
}

fn current_player_can_raise(state: &State) -> bool {
    let player_state = state.players_state[state.current_player as usize];
    let call_amount = f64::max(0.0, state.min_bet - player_state.bet_chips);
    let remaining_stake_after_call = player_state.stake - call_amount;

    remaining_stake_after_call > CHIP_EPSILON
}

// Modified to accept state parameter for verbose control
fn rank_hand(state: &State, private_cards: (Card, Card), public_cards: &Vec<Card>) -> (u64, u64, u64) {
    let mut cards = public_cards.clone();
    cards.append(&mut vec![private_cards.0, private_cards.1]);

    let min_rank = cards
        .iter()
        .copied()
        .combinations(5)
        .map(|comb| rank_card_combination(comb))
        .min()
        .unwrap();

    min_rank
}

fn rank_card_combination(cards: Vec<Card>) -> (u64, u64, u64) {
    let mut ordered_cards = cards.clone();
    ordered_cards.sort_by_key(|c| c.rank);
    let suits: Vec<CardSuit> = ordered_cards.iter().map(|c| c.suit).collect();
    let ranks: Vec<CardRank> = ordered_cards.iter().map(|c| c.rank).collect();

    let suit_duplicates: Vec<(usize, CardSuit)> = suits
        .iter()
        .copied()
        .dedup_with_count()
        .sorted_by_key(|(n, _)| n.clone())
        .rev()
        .collect();

    let rank_duplicates: Vec<(usize, CardRank)> = ranks
        .iter()
        .copied()
        .dedup_with_count()
        .sorted_by_key(|(n, _)| n.clone())
        .rev()
        .collect();

    let ranks_in_sequence = ranks
        .windows(2)
        .map(|x| x[1] as i32 - x[0] as i32)
        .all(|d| d == 1)
        || ranks
            == vec![
                CardRank::R2,
                CardRank::R3,
                CardRank::R4,
                CardRank::R5,
                CardRank::RA,
            ];

    // Royal flush: A, K, Q, J, 10, all the same suit.
    if ranks[..]
        == [
            CardRank::RT,
            CardRank::RJ,
            CardRank::RQ,
            CardRank::RK,
            CardRank::RA,
        ]
        && suit_duplicates[0].0 == 5
    {
        return (1, 0, 0_u64);
    }
    // Straight flush: Five cards in a sequence, all in the same suit.
    if ranks_in_sequence && suit_duplicates[0].0 == 5 {
        return (2, high_card_value(&ranks), 0_u64);
    }
    // 3. Four of a kind: All four cards of the same rank.
    if rank_duplicates[0].0 == 4 {
        let relevant_ranks = vec![rank_duplicates[0].1];
        return (3, high_card_value(&relevant_ranks), high_card_value(&ranks));
    }
    // 4. Full house: Three of a kind with a pair.
    if rank_duplicates[0].0 == 3 && rank_duplicates[1].0 == 2 {
        let relevant_ranks = vec![rank_duplicates[0].1];
        return (4, high_card_value(&relevant_ranks), high_card_value(&ranks));
    }
    // 5. Flush: Any five cards of the same suit, but not in a sequence.
    if suit_duplicates[0].0 == 5 {
        return (5, high_card_value(&ranks), 0_u64);
    }
    // 6. Straight: Five cards in a sequence, but not of the same suit.
    if ranks_in_sequence {
        return (6, high_card_value(&ranks), 0_u64);
    }
    // 7. Three of a kind: Three cards of the same rank.
    if rank_duplicates[0].0 == 3 {
        let relevant_ranks = vec![rank_duplicates[0].1];
        return (7, high_card_value(&relevant_ranks), high_card_value(&ranks));
    }
    // 8. Two pair: Two different pairs.
    if rank_duplicates[0].0 == 2 && rank_duplicates[1].0 == 2 {
        let relevant_ranks = vec![rank_duplicates[0].1, rank_duplicates[1].1];
        return (8, high_card_value(&relevant_ranks), high_card_value(&ranks));
    }
    // 9. Pair: Two cards of the same rank.
    if rank_duplicates[0].0 == 2 {
        let relevant_ranks = vec![rank_duplicates[0].1];
        return (9, high_card_value(&relevant_ranks), high_card_value(&ranks));
    }

    // 10. High Card: When you haven't made any of the hands above, the highest card plays.
    (10, high_card_value(&ranks), 0_u64)
}

fn high_card_value(ranks: &Vec<CardRank>) -> u64 {
    let mut value: u64 = 0;
    for (i, &r) in ranks.iter().sorted().enumerate() {
        value += (13_u64.pow(i as u32)) * (12 - r as u64);
    }
    value
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
            let initial_state = State::from_deck(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, deck, false);
            match initial_state {
                Ok(mut s) => {
                    for a in actions {
                        s = s.apply_action(a);
                    }
                },
                Err(_) => return Ok(())
            };

        }

        #[test]
        fn zero_sum_game(n_players in 2..26, seed: u64, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions in prop::collection::vec(Action::arbitrary_with(((), ())).prop_filter("Raise abs amount bellow 1e12",
        |a| a.amount.abs() < 1e12), 1..100)) {
            let initial_state = State::from_seed(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, seed, false);
            match initial_state {
                Ok(mut s) => {
                    for a in actions {
                        s = s.apply_action(a);
                        if s.final_state {
                            let sum_rewards = s.players_state.iter().map(|ps| ps.reward).fold(0_f64, |r1, r2| r1 + r2);
                            println!("sum_rewards = {sum_rewards}");
                            prop_assert!(sum_rewards < 1e-12);
                        }
                    }
                },
                Err(err) => {
                    println!("{}", err.msg);
                    prop_assert!(false);
                }
            };
        }

        #[test]
        fn call_and_check_no_legal_at_same_time(n_players in 2..26, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions in prop::collection::vec(Action::arbitrary_with(((), ())), 1..100)) {
            let initial_state = State::from_seed(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, 1234, false);
            match initial_state {
                Ok(mut s) => {
                    for a in actions {
                        s = s.apply_action(a);
                        if s.final_state {
                            prop_assert!(!(s.legal_actions.contains(&ActionEnum::Check) && s.legal_actions.contains(&ActionEnum::Call)));
                        }
                    }
                },
                Err(err) => {
                    println!("{}", err.msg);
                    prop_assert!(false);
                }
            };
        }

        #[test]
        fn illegal_raise_own_call(n_players in 2..26, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions in prop::collection::vec(Action::arbitrary_with(((), ())), 1..100)) {
            let initial_state = State::from_seed(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, 1234, false);
            match initial_state {
                Ok(mut s) => {
                    for a in actions {
                        s = s.apply_action(a);
                        let call_done = s.players_state[s.current_player as usize].last_stage_action == Some(ActionEnum::Call);
                        let not_other_raise = !s.players_state.iter().filter(|ps| ps.active).any(|ps| ps.last_stage_action == Some(ActionEnum::Raise));
                        if call_done && not_other_raise {
                            prop_assert!(!s.legal_actions.contains(&ActionEnum::Raise));
                        }
                    }
                },
                Err(err) => {
                    println!("{}", err.msg);
                    prop_assert!(false);
                }
            };
        }

        #[test]
        fn illegal_call_zero_bets(n_players in 2..26, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions in prop::collection::vec(Action::arbitrary_with(((), ())), 1..100)) {
            let initial_state = State::from_seed(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, 1234, false);
            match initial_state {
                Ok(mut s) => {
                    for a in actions {
                        s = s.apply_action(a);
                        prop_assert!(!(s.legal_actions.contains(&ActionEnum::Call) && s.min_bet == 0.0));
                    }
                },
                Err(err) => {
                    println!("{}", err.msg);
                    prop_assert!(false);
                }
            };
        }

        #[test]
        fn from_action_not_none(n_players in 2..26, sb in 0.5_f64..100.0_f64, bb_mult in 2..5, stake_mult in 100..1000, actions in prop::collection::vec(Action::arbitrary_with(((), ())), 1..100)) {
            let initial_state = State::from_seed(n_players as u64, 0, sb, sb * bb_mult as f64, sb * stake_mult as f64, 1234, false);
            match initial_state {
                Ok(mut s) => {
                    for a in actions {
                        s = s.apply_action(a);
                        prop_assert!(s.from_action != None);
                    }
                },
                Err(err) => {
                    println!("{}", err.msg);
                    prop_assert!(false);
                }
            };
        }
    }
}
