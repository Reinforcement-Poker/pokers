use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::sync::{broadcast, RwLock};
use tokio_tungstenite::{accept_async, tungstenite::Message};
use tracing::{error, info, warn};
use uuid::Uuid;

use crate::game_server::{GameConfig, GameServer, PlayerAction};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WebSocketMessage {
    pub message_type: String,
    pub data: serde_json::Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RegisterPlayerMessage {
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TakeSeatMessage {
    pub seat: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlayerActionMessage {
    pub action: String,
    pub amount: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameStateMessage {
    pub game_started: bool,
    pub players: HashMap<String, PlayerInfo>,
    pub community_cards: Vec<CardInfo>,
    pub pot: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlayerInfo {
    pub name: String,
    pub address: String,
    pub chips: f64,
    pub bet: f64,
    pub in_game: bool,
    pub on_move: bool,
    pub folded: bool,
    pub session_net_win_loss: f64,
    pub cards: Vec<CardInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CardInfo {
    pub suit: u8,
    pub rank: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OnMoveMessage {
    pub seat: u8,
    pub address: String,
    pub name: String,
    pub chips: f64,
    pub bet: f64,
    pub on_move: bool,
    pub in_game: bool,
    pub folded: bool,
    pub cards: Vec<CardInfo>,
    pub max_bet_on_table: f64,
    pub can_check: bool,
    pub call_amount: f64,
    pub min_bet_to_total_value: f64,
    pub min_raise_to_total_bet: f64,
    pub pot_size: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PotUpdateMessage {
    pub main_pot: f64,
    pub side_pots: Vec<f64>,
    pub player_bets: HashMap<String, f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HandWinningsMessage {
    pub community_cards: Vec<CardInfo>,
    pub winnings: Vec<WinningInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WinningInfo {
    pub seat_id: u8,
    pub player_name: String,
    pub amount_won: f64,
    pub pot_description: String,
    pub hand_description: String,
    pub hole_cards: Vec<CardInfo>,
}

pub type ClientId = String;
pub type ClientSender = tokio::sync::mpsc::UnboundedSender<Message>;

#[derive(Clone)]
pub struct WebSocketServer {
    clients: Arc<RwLock<HashMap<ClientId, ClientSender>>>,
    game_server: Arc<RwLock<GameServer>>,
    broadcast_sender: broadcast::Sender<String>,
}

impl WebSocketServer {
    #[allow(dead_code)]
    pub fn new() -> Self {
        let (broadcast_sender, _) = broadcast::channel(1000);

        Self {
            clients: Arc::new(RwLock::new(HashMap::new())),
            game_server: Arc::new(RwLock::new(GameServer::new(None))),
            broadcast_sender,
        }
    }

    pub fn new_with_config(config: GameConfig) -> Self {
        let (broadcast_sender, _) = broadcast::channel(1000);

        Self {
            clients: Arc::new(RwLock::new(HashMap::new())),
            game_server: Arc::new(RwLock::new(GameServer::new(Some(config)))),
            broadcast_sender,
        }
    }

    pub async fn start(&self, addr: SocketAddr) -> Result<(), Box<dyn std::error::Error>> {
        let listener = tokio::net::TcpListener::bind(addr).await?;
        info!("WebSocket server listening on: {}", addr);

        while let Ok((stream, peer_addr)) = listener.accept().await {
            let clients = self.clients.clone();
            let game_server = self.game_server.clone();
            let broadcast_sender = self.broadcast_sender.clone();

            tokio::spawn(async move {
                if let Err(e) =
                    handle_connection(stream, peer_addr, clients, game_server, broadcast_sender)
                        .await
                {
                    error!("Error handling connection from {}: {}", peer_addr, e);
                }
            });
        }

        Ok(())
    }

    pub async fn broadcast_message(&self, message: &str) {
        if let Err(e) = self.broadcast_sender.send(message.to_string()) {
            warn!("Failed to broadcast message: {}", e);
        }
    }

    pub async fn broadcast_game_state(&self, state: GameStateMessage) {
        let message = WebSocketMessage {
            message_type: "gameState".to_string(),
            data: serde_json::to_value(state).unwrap_or_default(),
        };

        if let Ok(json) = serde_json::to_string(&message) {
            self.broadcast_message(&json).await;
        }
    }

    pub async fn broadcast_on_move(&self, on_move: OnMoveMessage) {
        let message = WebSocketMessage {
            message_type: "onmove".to_string(),
            data: serde_json::to_value(on_move).unwrap_or_default(),
        };

        if let Ok(json) = serde_json::to_string(&message) {
            self.broadcast_message(&json).await;
        }
    }

    #[allow(dead_code)]
    pub async fn broadcast_pot_update(&self, pot_update: PotUpdateMessage) {
        let message = WebSocketMessage {
            message_type: "potUpdate".to_string(),
            data: serde_json::to_value(pot_update).unwrap_or_default(),
        };

        if let Ok(json) = serde_json::to_string(&message) {
            self.broadcast_message(&json).await;
        }
    }

    pub async fn broadcast_winnings(&self, winnings: HandWinningsMessage) {
        let message = WebSocketMessage {
            message_type: "handWinnings".to_string(),
            data: serde_json::to_value(winnings).unwrap_or_default(),
        };

        if let Ok(json) = serde_json::to_string(&message) {
            self.broadcast_message(&json).await;
        }
    }
}

async fn handle_connection(
    stream: tokio::net::TcpStream,
    peer_addr: SocketAddr,
    clients: Arc<RwLock<HashMap<ClientId, ClientSender>>>,
    game_server: Arc<RwLock<GameServer>>,
    broadcast_sender: broadcast::Sender<String>,
) -> Result<(), Box<dyn std::error::Error>> {
    let client_id = Uuid::new_v4().to_string();
    info!(
        "New WebSocket connection from {} with ID {}",
        peer_addr, client_id
    );

    let ws_stream = accept_async(stream).await?;
    let (mut ws_sender, mut ws_receiver) = ws_stream.split();

    let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();

    // Add client to the clients map
    {
        let mut clients_guard = clients.write().await;
        clients_guard.insert(client_id.clone(), tx);
    }

    let mut broadcast_receiver = broadcast_sender.subscribe();

    // Spawn task to handle outgoing messages
    let client_id_clone = client_id.clone();
    let clients_clone = clients.clone();
    let outgoing_task = tokio::spawn(async move {
        loop {
            tokio::select! {
                // Handle direct messages to this client
                msg = rx.recv() => {
                    match msg {
                        Some(message) => {
                            if ws_sender.send(message).await.is_err() {
                                break;
                            }
                        }
                        None => break,
                    }
                }
                // Handle broadcast messages
                broadcast_msg = broadcast_receiver.recv() => {
                    match broadcast_msg {
                        Ok(msg) => {
                            if ws_sender.send(Message::Text(msg)).await.is_err() {
                                break;
                            }
                        }
                        Err(_) => break,
                    }
                }
            }
        }

        // Remove client when connection closes
        let mut clients_guard = clients_clone.write().await;
        clients_guard.remove(&client_id_clone);
        info!("Client {} disconnected", client_id_clone);
    });

    // Handle incoming messages
    while let Some(msg) = ws_receiver.next().await {
        match msg {
            Ok(Message::Text(text)) => {
                if let Err(e) = handle_message(&text, &client_id, &game_server).await {
                    error!("Error handling message from {}: {}", client_id, e);
                }
            }
            Ok(Message::Close(_)) => {
                info!("Client {} sent close message", client_id);
                break;
            }
            Err(e) => {
                error!("WebSocket error for client {}: {}", client_id, e);
                break;
            }
            _ => {}
        }
    }

    // Notify game server about player disconnection
    {
        let mut game = game_server.write().await;
        game.player_disconnected(&client_id).await;
    }

    outgoing_task.abort();
    Ok(())
}

async fn handle_message(
    text: &str,
    client_id: &str,
    game_server: &Arc<RwLock<GameServer>>,
) -> Result<(), Box<dyn std::error::Error>> {
    let message: WebSocketMessage = serde_json::from_str(text)?;

    info!(
        "Received message from {}: {}",
        client_id, message.message_type
    );

    let mut game = game_server.write().await;

    match message.message_type.as_str() {
        "registerPlayer" => {
            let register_msg: RegisterPlayerMessage = serde_json::from_value(message.data)?;
            game.register_player(&register_msg.name, client_id).await?;
        }
        "takeSeat" => {
            let seat_msg: TakeSeatMessage = serde_json::from_value(message.data)?;
            game.seat_player(client_id, seat_msg.seat).await?;
        }
        "startGame" => {
            game.start_game().await?;
        }
        "fold" => {
            game.handle_action(client_id, PlayerAction::Fold).await?;
        }
        "check" => {
            game.handle_action(client_id, PlayerAction::Check).await?;
        }
        "call" => {
            game.handle_action(client_id, PlayerAction::Call).await?;
        }
        "raise" => {
            let action_msg: PlayerActionMessage = serde_json::from_value(message.data)?;
            let amount = action_msg.amount.unwrap_or(0.0);
            game.handle_action(client_id, PlayerAction::Raise(amount))
                .await?;
        }
        "bet" => {
            let action_msg: PlayerActionMessage = serde_json::from_value(message.data)?;
            let amount = action_msg.amount.unwrap_or(0.0);
            game.handle_action(client_id, PlayerAction::Bet(amount))
                .await?;
        }
        _ => {
            warn!("Unknown message type: {}", message.message_type);
        }
    }

    Ok(())
}
