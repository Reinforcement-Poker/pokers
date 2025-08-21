# Poker WebSocket Server

This is a Rust-based WebSocket poker game server that implements Texas Hold'em poker logic using the existing game engine.

## Features

- WebSocket-based real-time communication
- Player registration and seat management
- Complete poker game flow (preflop, flop, turn, river, showdown)
- Action handling (fold, check, call, raise, bet)
- Game state synchronization
- JSON message protocol compatible with the C++ server interface

## Building and Running

### Build the WebSocket server
```bash
cargo build --features websocket --bin websocket_server
```

### Run the server
```bash
cargo run --features websocket --bin websocket_server [port]
```

Default port is 8888 if not specified.

Example:
```bash
cargo run --features websocket --bin websocket_server 9000
```

## WebSocket API

The server uses JSON messages over WebSocket connections. All messages follow this format:

```json
{
  "messageType": "actionName",
  "data": { ... }
}
```

### Client to Server Messages

#### Register Player
```json
{
  "messageType": "registerPlayer",
  "data": {
    "name": "PlayerName"
  }
}
```

#### Take Seat
```json
{
  "messageType": "takeSeat",
  "data": {
    "seat": 1
  }
}
```

#### Start Game
```json
{
  "messageType": "startGame",
  "data": {}
}
```

#### Player Actions
```json
{
  "messageType": "fold",
  "data": {}
}
```

```json
{
  "messageType": "check",
  "data": {}
}
```

```json
{
  "messageType": "call",
  "data": {}
}
```

```json
{
  "messageType": "raise",
  "data": {
    "amount": 100.0
  }
}
```

```json
{
  "messageType": "bet",
  "data": {
    "amount": 50.0
  }
}
```

### Server to Client Messages

#### Game State
Broadcasted to all clients when game state changes:
```json
{
  "messageType": "gameState",
  "data": {
    "gameStarted": true,
    "players": {
      "1": {
        "name": "Player1",
        "address": "client-id",
        "chips": 1000.0,
        "bet": 10.0,
        "inGame": true,
        "onMove": false,
        "folded": false,
        "sessionNetWinLoss": 0.0,
        "cards": [
          {"suit": 0, "rank": 14},
          {"suit": 1, "rank": 13}
        ]
      }
    },
    "communityCards": [
      {"suit": 2, "rank": 12},
      {"suit": 3, "rank": 11},
      {"suit": 0, "rank": 10}
    ],
    "pot": 50.0
  }
}
```

#### On Move
Sent when it's a player's turn to act:
```json
{
  "messageType": "onmove",
  "data": {
    "seat": 1,
    "address": "client-id",
    "name": "Player1",
    "chips": 1000.0,
    "bet": 10.0,
    "onMove": true,
    "inGame": true,
    "folded": false,
    "cards": [...],
    "maxBetOnTable": 20.0,
    "canCheck": false,
    "callAmount": 10.0,
    "minBetToTotalValue": 20.0,
    "minRaiseToTotalBet": 30.0,
    "potSize": 50.0
  }
}
```

#### Hand Winnings
Sent at the end of each hand:
```json
{
  "messageType": "handWinnings",
  "data": {
    "communityCards": [...],
    "winnings": [
      {
        "seatId": 1,
        "playerName": "Player1",
        "amountWon": 100.0,
        "potDescription": "Main Pot",
        "handDescription": "Winner",
        "holeCards": [...]
      }
    ]
  }
}
```

## Game Configuration

The server uses these default settings:
- Max players: 6
- Default stack size: 1000 chips
- Small blind: 5 chips
- Big blind: 10 chips
- Ante: 0 chips

## Architecture

- `src/main.rs` - Entry point and server initialization
- `src/websocket_server.rs` - WebSocket connection handling and message routing
- `src/game_server.rs` - Game logic and state management
- `src/game_logic.rs` - Core poker game engine (reused from existing Python module)
- `src/state/` - Game state structures and types

The server integrates with the existing Rust poker game engine to provide WebSocket-based multiplayer functionality that's compatible with the C++ server's interface.