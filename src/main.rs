use std::net::SocketAddr;
use std::sync::Arc;
use tracing::{error, info};
use tracing_subscriber::fmt;

mod game_logic;
mod game_server;
mod state;
mod websocket_server;

use game_server::GameConfig;
use websocket_server::WebSocketServer;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize tracing
    fmt::init();

    // Parse command line arguments for port
    let args: Vec<String> = std::env::args().collect();
    let port = if args.len() > 1 {
        args[1].parse::<u16>().unwrap_or(9000)
    } else {
        9000
    };

    let addr: SocketAddr = format!("127.0.0.1:{}", port).parse()?;

    // Create game configuration
    let config = GameConfig {
        max_players: 6,
        default_stack_size: 1000.0,
        small_blind: 5.0,
        big_blind: 10.0,
        ante: 0.0,
    };

    // Create WebSocket server with config
    let ws_server = Arc::new(WebSocketServer::new_with_config(config));

    info!("Starting Poker WebSocket Server on {}", addr);

    // Start the server
    if let Err(e) = ws_server.start(addr).await {
        error!("Server error: {}", e);
    }

    Ok(())
}
