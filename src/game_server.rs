use std::collections::HashMap;
use std::sync::Arc;
use tracing::info;

use crate::state::action::{Action, ActionEnum};
use crate::state::card::Card;
use crate::state::State;
use crate::websocket_server::{
    CardInfo, GameStateMessage, HandWinningsMessage, OnMoveMessage, PlayerInfo, WebSocketServer,
    WinningInfo,
};

#[derive(Debug, Clone)]
pub enum PlayerAction {
    Fold,
    Check,
    Call,
    Raise(f64),
    Bet(f64),
}

#[derive(Debug, Clone)]
pub struct GamePlayer {
    pub id: String,
    pub name: String,
    pub seat: Option<u8>,
    pub chips: f64,
    pub connected: bool,
    pub starting_session_chips: f64,
}

impl GamePlayer {
    pub fn new(id: String, name: String, initial_chips: f64) -> Self {
        Self {
            id,
            name,
            seat: None,
            chips: initial_chips,
            connected: true,
            starting_session_chips: initial_chips,
        }
    }
}

#[derive(Clone)]
pub struct GameServer {
    players: HashMap<String, GamePlayer>,
    seats: HashMap<u8, String>, // seat number -> player_id
    game_state: Option<State>,
    websocket_server: Option<Arc<WebSocketServer>>,
    game_config: GameConfig,
    dealer_seat: u8,
    game_running: bool,
}

#[derive(Debug, Clone)]
pub struct GameConfig {
    pub max_players: u8,
    pub default_stack_size: f64,
    pub small_blind: f64,
    pub big_blind: f64,
    #[allow(dead_code)]
    pub ante: f64,
}

impl Default for GameConfig {
    fn default() -> Self {
        Self {
            max_players: 6,
            default_stack_size: 1000.0,
            small_blind: 5.0,
            big_blind: 10.0,
            ante: 0.0,
        }
    }
}

impl GameServer {
    pub fn new(config: Option<GameConfig>) -> Self {
        Self {
            players: HashMap::new(),
            seats: HashMap::new(),
            game_state: None,
            websocket_server: None,
            game_config: config.unwrap_or_default(),
            dealer_seat: 1,
            game_running: false,
        }
    }

    #[allow(dead_code)]
    pub fn new_with_websocket(config: Option<GameConfig>, ws_server: Arc<WebSocketServer>) -> Self {
        let mut server = Self::new(config);
        server.websocket_server = Some(ws_server);
        server
    }

    pub async fn register_player(
        &mut self,
        name: &str,
        player_id: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        // Check if player already exists (reconnection)
        if let Some(existing_player) = self.players.get_mut(player_id) {
            existing_player.connected = true;
            existing_player.name = name.to_string();
            info!("Player {} reconnected with ID {}", name, player_id);
        } else {
            // Check if name is already taken by another connected player
            let name_taken = self.players.values().any(|p| p.name == name && p.connected);

            if name_taken {
                return Err(format!("Name '{}' is already taken", name).into());
            }

            let player = GamePlayer::new(
                player_id.to_string(),
                name.to_string(),
                self.game_config.default_stack_size,
            );

            self.players.insert(player_id.to_string(), player);
            info!("New player {} registered with ID {}", name, player_id);
        }

        self.broadcast_game_state().await;
        Ok(())
    }

    pub async fn seat_player(
        &mut self,
        player_id: &str,
        seat: u8,
    ) -> Result<(), Box<dyn std::error::Error>> {
        if seat < 1 || seat > self.game_config.max_players {
            return Err(format!("Invalid seat number: {}", seat).into());
        }

        if self.seats.contains_key(&seat) {
            return Err(format!("Seat {} is already occupied", seat).into());
        }

        let player = self.players.get_mut(player_id).ok_or("Player not found")?;

        // Remove player from current seat if they have one
        if let Some(current_seat) = player.seat {
            self.seats.remove(&current_seat);
        }

        player.seat = Some(seat);
        self.seats.insert(seat, player_id.to_string());

        info!("Player {} took seat {}", player.name, seat);
        self.broadcast_game_state().await;
        Ok(())
    }

    pub async fn start_game(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        if self.seats.len() < 2 {
            return Err("Need at least 2 players to start the game".into());
        }

        let seated_players = self.seats.len() as u64;
        let button_player_id = self
            .seats
            .get(&self.dealer_seat)
            .ok_or("No player at dealer seat")?;
        let _button_player = self
            .players
            .get(button_player_id)
            .ok_or("Button player not found")?;

        // Create deck and initialize game state
        let deck = Card::collect();

        let game_state = State::from_deck(
            seated_players,
            (self.dealer_seat - 1) as u64, // Convert to 0-indexed
            self.game_config.small_blind,
            self.game_config.big_blind,
            self.game_config.default_stack_size,
            deck,
            false, // verbose
        )
        .map_err(|e| format!("Failed to create game state: {:?}", e))?;

        self.game_state = Some(game_state);
        self.game_running = true;

        info!("Game started with {} players", seated_players);
        self.broadcast_game_state().await;
        self.broadcast_current_player_turn().await;

        Ok(())
    }

    pub async fn handle_action(
        &mut self,
        player_id: &str,
        action: PlayerAction,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let (game_action, player_name) = {
            let game_state = self.game_state.as_ref().ok_or("No active game")?;

            let player = self.players.get(player_id).ok_or("Player not found")?;

            let seat = player.seat.ok_or("Player is not seated")?;

            // Check if it's the player's turn
            let current_player_seat = (game_state.current_player + 1) as u8; // Convert to 1-indexed
            if seat != current_player_seat {
                return Err("Not your turn".into());
            }

            // Convert PlayerAction to game logic Action
            let game_action = match action {
                PlayerAction::Fold => Action::new(ActionEnum::Fold, 0.0),
                PlayerAction::Check => Action::new(ActionEnum::Check, 0.0),
                PlayerAction::Call => Action::new(ActionEnum::Call, 0.0),
                PlayerAction::Raise(amount) => Action::new(ActionEnum::BetRaise, amount),
                PlayerAction::Bet(amount) => Action::new(ActionEnum::BetRaise, amount),
            };

            (game_action, player.name.clone())
        };

        // Apply action to game state
        if let Some(game_state) = self.game_state.take() {
            let new_state = game_state.apply_action(game_action);
            self.game_state = Some(new_state);
        }

        // Sync player chips from game state
        self.sync_player_chips_from_game_state();

        info!("Player {} performed action: {:?}", player_name, action);

        self.broadcast_game_state().await;

        // Check if game ended
        if let Some(ref state) = self.game_state {
            if state.final_state {
                self.handle_game_end().await?;
            } else {
                self.broadcast_current_player_turn().await;
            }
        }

        Ok(())
    }

    pub async fn player_disconnected(&mut self, player_id: &str) {
        if let Some(player) = self.players.get_mut(player_id) {
            player.connected = false;
            info!("Player {} disconnected", player.name);
            self.broadcast_game_state().await;
        }
    }

    fn sync_player_chips_from_game_state(&mut self) {
        if let Some(ref state) = self.game_state {
            for (seat, player_id) in &self.seats {
                if let Some(player) = self.players.get_mut(player_id) {
                    let player_state_index = (*seat - 1) as usize;
                    if let Some(player_state) = state.players_state.get(player_state_index) {
                        player.chips = player_state.stake + player_state.bet_chips;
                    }
                }
            }
        }
    }

    async fn handle_game_end(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        if let Some(ref state) = self.game_state {
            // Calculate winnings and update player chips
            for (seat, player_id) in &self.seats {
                if let Some(player) = self.players.get_mut(player_id) {
                    let player_state_index = (*seat - 1) as usize;
                    if let Some(player_state) = state.players_state.get(player_state_index) {
                        let total_reward = player_state.stake + player_state.reward;
                        player.chips = total_reward.max(0.0);
                    }
                }
            }

            self.broadcast_hand_winnings().await;
        }

        self.game_running = false;
        self.game_state = None;

        // Rotate dealer
        self.rotate_dealer();

        info!("Game ended");
        Ok(())
    }

    fn rotate_dealer(&mut self) {
        let seated_players: Vec<u8> = self.seats.keys().copied().collect();
        if seated_players.is_empty() {
            return;
        }

        let mut sorted_seats = seated_players;
        sorted_seats.sort();

        if let Some(current_index) = sorted_seats.iter().position(|&s| s == self.dealer_seat) {
            let next_index = (current_index + 1) % sorted_seats.len();
            self.dealer_seat = sorted_seats[next_index];
        } else {
            self.dealer_seat = sorted_seats[0];
        }
    }

    async fn broadcast_game_state(&self) {
        if let Some(ref ws_server) = self.websocket_server {
            let mut players_info = HashMap::new();

            for seat in 1..=self.game_config.max_players {
                if let Some(player_id) = self.seats.get(&seat) {
                    if let Some(player) = self.players.get(player_id) {
                        let player_cards = self.get_player_cards(seat);

                        let player_info = PlayerInfo {
                            name: player.name.clone(),
                            address: player.id.clone(),
                            chips: player.chips,
                            bet: self.get_player_bet(seat),
                            in_game: player.connected && player.seat.is_some(),
                            on_move: self.is_player_on_move(seat),
                            folded: self.is_player_folded(seat),
                            session_net_win_loss: player.chips - player.starting_session_chips,
                            cards: player_cards,
                        };

                        players_info.insert(seat.to_string(), player_info);
                    }
                }
            }

            let community_cards = self.get_community_cards();
            let pot = self.get_pot_size();

            let game_state_msg = GameStateMessage {
                game_started: self.game_running,
                players: players_info,
                community_cards,
                pot,
            };

            ws_server.broadcast_game_state(game_state_msg).await;
        }
    }

    async fn broadcast_current_player_turn(&self) {
        if let Some(ref state) = self.game_state {
            if let Some(ref ws_server) = self.websocket_server {
                let current_seat = (state.current_player + 1) as u8;

                if let Some(player_id) = self.seats.get(&current_seat) {
                    if let Some(player) = self.players.get(player_id) {
                        let player_cards = self.get_player_cards(current_seat);

                        let on_move_msg = OnMoveMessage {
                            seat: current_seat,
                            address: player.id.clone(),
                            name: player.name.clone(),
                            chips: player.chips,
                            bet: self.get_player_bet(current_seat),
                            on_move: true,
                            in_game: true,
                            folded: false,
                            cards: player_cards,
                            max_bet_on_table: self.get_max_bet(),
                            can_check: self.can_player_check(current_seat),
                            call_amount: self.get_call_amount(current_seat),
                            min_bet_to_total_value: state.min_bet,
                            min_raise_to_total_bet: self.get_min_raise_amount(current_seat),
                            pot_size: state.pot,
                        };

                        ws_server.broadcast_on_move(on_move_msg).await;
                    }
                }
            }
        }
    }

    async fn broadcast_hand_winnings(&self) {
        if let Some(ref ws_server) = self.websocket_server {
            let community_cards = self.get_community_cards();
            let winnings = self.calculate_winnings();

            let winnings_msg = HandWinningsMessage {
                community_cards,
                winnings,
            };

            ws_server.broadcast_winnings(winnings_msg).await;
        }
    }

    fn get_player_cards(&self, seat: u8) -> Vec<CardInfo> {
        if let Some(ref state) = self.game_state {
            let player_index = (seat - 1) as usize;
            if let Some(player_state) = state.players_state.get(player_index) {
                return vec![
                    CardInfo {
                        suit: player_state.hand.0.suit as u8,
                        rank: player_state.hand.0.rank as u8 + 2, // Convert to 2-14 range
                    },
                    CardInfo {
                        suit: player_state.hand.1.suit as u8,
                        rank: player_state.hand.1.rank as u8 + 2,
                    },
                ];
            }
        }
        Vec::new()
    }

    fn get_community_cards(&self) -> Vec<CardInfo> {
        if let Some(ref state) = self.game_state {
            return state
                .public_cards
                .iter()
                .map(|card| CardInfo {
                    suit: card.suit as u8,
                    rank: card.rank as u8 + 2,
                })
                .collect();
        }
        Vec::new()
    }

    fn get_player_bet(&self, seat: u8) -> f64 {
        if let Some(ref state) = self.game_state {
            let player_index = (seat - 1) as usize;
            if let Some(player_state) = state.players_state.get(player_index) {
                return player_state.bet_chips;
            }
        }
        0.0
    }

    fn get_pot_size(&self) -> f64 {
        self.game_state.as_ref().map(|s| s.pot).unwrap_or(0.0)
    }

    fn is_player_on_move(&self, seat: u8) -> bool {
        if let Some(ref state) = self.game_state {
            return (state.current_player + 1) as u8 == seat;
        }
        false
    }

    fn is_player_folded(&self, seat: u8) -> bool {
        if let Some(ref state) = self.game_state {
            let player_index = (seat - 1) as usize;
            if let Some(player_state) = state.players_state.get(player_index) {
                return !player_state.active;
            }
        }
        false
    }

    fn get_max_bet(&self) -> f64 {
        if let Some(ref state) = self.game_state {
            return state
                .players_state
                .iter()
                .map(|ps| ps.bet_chips)
                .fold(0.0, f64::max);
        }
        0.0
    }

    fn can_player_check(&self, seat: u8) -> bool {
        if let Some(ref state) = self.game_state {
            let player_index = (seat - 1) as usize;
            if let Some(player_state) = state.players_state.get(player_index) {
                return player_state.bet_chips >= state.min_bet;
            }
        }
        false
    }

    fn get_call_amount(&self, seat: u8) -> f64 {
        if let Some(ref state) = self.game_state {
            let player_index = (seat - 1) as usize;
            if let Some(player_state) = state.players_state.get(player_index) {
                let call_amount = state.min_bet - player_state.bet_chips;
                return call_amount.max(0.0).min(player_state.stake);
            }
        }
        0.0
    }

    fn get_min_raise_amount(&self, _seat: u8) -> f64 {
        if let Some(ref state) = self.game_state {
            return state.min_bet + state.bb;
        }
        0.0
    }

    fn calculate_winnings(&self) -> Vec<WinningInfo> {
        let mut winnings = Vec::new();

        if let Some(ref state) = self.game_state {
            for (seat, player_id) in &self.seats {
                if let Some(player) = self.players.get(player_id) {
                    let player_index = (*seat - 1) as usize;
                    if let Some(player_state) = state.players_state.get(player_index) {
                        if player_state.reward > 0.0 {
                            let hole_cards = vec![
                                CardInfo {
                                    suit: player_state.hand.0.suit as u8,
                                    rank: player_state.hand.0.rank as u8 + 2,
                                },
                                CardInfo {
                                    suit: player_state.hand.1.suit as u8,
                                    rank: player_state.hand.1.rank as u8 + 2,
                                },
                            ];

                            winnings.push(WinningInfo {
                                seat_id: *seat,
                                player_name: player.name.clone(),
                                amount_won: player_state.reward,
                                pot_description: "Main Pot".to_string(),
                                hand_description: "Winner".to_string(), // TODO: Implement proper hand evaluation
                                hole_cards,
                            });
                        }
                    }
                }
            }
        }

        winnings
    }
}
