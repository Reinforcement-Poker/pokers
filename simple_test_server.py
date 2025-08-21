#!/usr/bin/env python3
"""
Simple WebSocket server test for quick validation.

This is a lightweight test that can be used to quickly verify the server is working.
"""

import asyncio
import json
import websockets
import sys

async def test_simple_connection(server_url="ws://localhost:9000"):
    """Simple test to connect and register a player."""
    print(f"Connecting to {server_url}...")

    try:
        async with websockets.connect(server_url) as websocket:
            print("✓ Connected successfully")

            # Register player
            register_msg = {
                "messageType": "registerPlayer",
                "data": {"name": "TestPlayer"}
            }
            await websocket.send(json.dumps(register_msg))
            print("✓ Sent registration message")

            # Take a seat
            seat_msg = {
                "messageType": "takeSeat",
                "data": {"seat": 1}
            }
            await websocket.send(json.dumps(seat_msg))
            print("✓ Sent take seat message")

            # Listen for a few messages
            print("Listening for server responses...")
            try:
                for i in range(3):  # Listen for up to 3 messages
                    message = await asyncio.wait_for(websocket.recv(), timeout=2.0)
                    data = json.loads(message)
                    print(f"✓ Received: {data.get('messageType', 'unknown')}")
            except asyncio.TimeoutError:
                print("⚠ No more messages received (timeout)")

            print("✓ Simple test completed successfully")

    except ConnectionRefusedError:
        print("✗ Connection refused - is the server running?")
        print("Start server with: ./target/release/websocket_server [port]")
        return False
    except Exception as e:
        print(f"✗ Test failed: {e}")
        return False

    return True

async def test_two_players(server_url="ws://localhost:9000"):
    """Test with two players to see if a game can start."""
    print(f"\n=== Testing Two Players ===")

    try:
        # Connect two players
        async with websockets.connect(server_url) as ws1, \
                   websockets.connect(server_url) as ws2:

            print("✓ Both players connected")

            # Register players
            await ws1.send(json.dumps({"messageType": "registerPlayer", "data": {"name": "Alice"}}))
            await ws2.send(json.dumps({"messageType": "registerPlayer", "data": {"name": "Bob"}}))
            print("✓ Players registered")

            # Take seats
            await ws1.send(json.dumps({"messageType": "takeSeat", "data": {"seat": 1}}))
            await ws2.send(json.dumps({"messageType": "takeSeat", "data": {"seat": 2}}))
            print("✓ Players seated")

            # Start game
            await ws1.send(json.dumps({"messageType": "startGame", "data": {}}))
            print("✓ Game start requested")

            # Listen for game state changes
            print("Listening for game events...")
            try:
                for i in range(10):  # Listen for up to 10 messages
                    message = await asyncio.wait_for(ws1.recv(), timeout=1.0)
                    data = json.loads(message)
                    msg_type = data.get('messageType', 'unknown')
                    print(f"  Player 1 received: {msg_type}")

                    if msg_type == "gameState":
                        game_data = data.get('data', {})
                        players = game_data.get('players', {})
                        pot = game_data.get('pot', 0)
                        print(f"    Game state: {len(players)} players, pot: {pot}")
                    elif msg_type == "onmove":
                        seat = data.get('data', {}).get('seat')
                        print(f"    It's player {seat}'s turn")
                        # Send a check action
                        if data.get('data', {}).get('canCheck', False):
                            await ws1.send(json.dumps({"messageType": "check", "data": {}}))
                            print("    Sent check action")

            except asyncio.TimeoutError:
                print("⚠ No more messages (timeout)")

            print("✓ Two player test completed")
            return True

    except Exception as e:
        print(f"✗ Two player test failed: {e}")
        return False

async def main():
    server_url = "ws://localhost:9000"

    if len(sys.argv) > 1:
        port = sys.argv[1]
        server_url = f"ws://localhost:{port}"

    print(f"Testing WebSocket Poker Server at {server_url}")
    print("Make sure the server is running first!")
    print()

    # Run simple test
    success1 = await test_simple_connection(server_url)

    if success1:
        # Run two player test
        await test_two_players(server_url)

    print("\nTest completed!")

if __name__ == "__main__":
    asyncio.run(main())
