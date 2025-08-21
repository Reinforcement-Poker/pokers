# WebSocket Server Testing

This directory contains Python test scripts to verify the functionality of the Rust WebSocket poker server.

## Setup

1. Install Python dependencies:
```bash
pip install websockets
# or
pip install -r test_requirements.txt
```

2. Build and start the WebSocket server:
```bash
cargo build --features websocket --bin websocket_server
cargo run --features websocket --bin websocket_server [port]
```

## Test Scripts

### 1. Simple Test (`simple_test_server.py`)
Quick validation test that:
- Connects to the server
- Registers a player
- Takes a seat
- Tests basic message exchange
- Tests two-player game start

Usage:
```bash
python simple_test_server.py [port]
```

### 2. Comprehensive Test (`test_websocket_server.py`)
Full test suite that:
- Tests all message types
- Simulates complete game flow
- Tests multiple players
- Includes auto-play logic
- Runs for extended periods

Usage:
```bash
python test_websocket_server.py [port]
```

## Example Test Run

1. Start the server:
```bash
cargo run --features websocket --bin websocket_server 9000
```

2. In another terminal, run the simple test:
```bash
python simple_test_server.py 9000
```

3. For comprehensive testing:
```bash
python test_websocket_server.py 9000
```

## Expected Output

The simple test should show:
```
✓ Connected successfully
✓ Sent registration message
✓ Sent take seat message
✓ Received: gameState
✓ Simple test completed successfully
```

## Troubleshooting

- **Connection refused**: Make sure the server is running on the correct port
- **Import errors**: Install websockets with `pip install websockets`
- **Timeout errors**: The server might be processing - this is normal
- **Action errors**: Some actions may fail if it's not the player's turn - this is expected behavior
