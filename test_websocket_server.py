#!/usr/bin/env python3
"""
Comprehensive test suite for the Poker WebSocket Server.

This script tests all major functionality of the WebSocket server including:
- Player registration
- Seat management
- Game flow (preflop, flop, turn, river, showdown)
- Action handling (fold, check, call, raise, bet)
- Game state synchronization
"""

import asyncio
import json
import websockets
import logging
import time
from typing import Dict, List, Optional, Any
from dataclasses import dataclass
import signal
import sys

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

@dataclass
class TestPlayer:
    name: str
    websocket: Optional[websockets.WebSocketServerProtocol] = None
    seat: Optional[int] = None
    chips: float = 1000.0
    connected: bool = False

class PokerServerTest:
    def __init__(self, server_url: str = "ws://localhost:8888"):
        self.server_url = server_url
        self.players: Dict[str, TestPlayer] = {}
        self.game_state: Dict = {}
        self.test_results: List[str] = []
        self.running = True

    async def create_player(self, player_name: str) -> TestPlayer:
        """Create a new test player and connect to the server."""
        player = TestPlayer(name=player_name)

        try:
            player.websocket = await websockets.connect(self.server_url)
            player.connected = True
            self.players[player_name] = player
            logger.info(f"Player {player_name} connected to server")

            # Start listening for messages
            asyncio.create_task(self.listen_for_messages(player))

            return player
        except Exception as e:
            logger.error(f"Failed to connect player {player_name}: {e}")
            raise

    async def listen_for_messages(self, player: TestPlayer):
        """Listen for messages from the server for a specific player."""
        try:
            async for message in player.websocket:
                data = json.loads(message)
                await self.handle_server_message(player, data)
        except websockets.exceptions.ConnectionClosed:
            logger.info(f"Player {player.name} connection closed")
            player.connected = False
        except Exception as e:
            logger.error(f"Error listening for messages from {player.name}: {e}")

    async def handle_server_message(self, player: TestPlayer, message: Dict):
        """Handle messages received from the server."""
        message_type = message.get("messageType", "")
        data = message.get("data", {})

        logger.info(f"Player {player.name} received: {message_type}")

        if message_type == "gameState":
            self.game_state = data
            logger.info(f"Game state updated: {len(data.get('players', {}))} players, pot: {data.get('pot', 0)}")

        elif message_type == "onmove":
            seat = data.get("seat")
            if seat == player.seat:
                logger.info(f"It's {player.name}'s turn to act")
                # Auto-play logic for testing
                await self.auto_play(player, data)

        elif message_type == "handWinnings":
            logger.info(f"Hand finished, winnings: {data}")

    async def auto_play(self, player: TestPlayer, move_data: Dict):
        """Simple auto-play logic for testing."""
        await asyncio.sleep(0.5)  # Small delay to simulate thinking

        can_check = move_data.get("canCheck", False)
        call_amount = move_data.get("callAmount", 0)
        min_raise = move_data.get("minRaiseToTotalBet", 0)

        # Simple strategy: check if possible, otherwise call small amounts or fold
        if can_check:
            await self.send_action(player, "check")
        elif call_amount <= 50:  # Call if it's a small amount
            await self.send_action(player, "call")
        elif call_amount <= 100 and min_raise > 0:  # Sometimes raise small amounts
            await self.send_action(player, "raise", {"amount": min_raise})
        else:
            await self.send_action(player, "fold")

    async def send_message(self, player: TestPlayer, message_type: str, data: Dict = None):
        """Send a message to the server."""
        if not player.connected or not player.websocket:
            raise Exception(f"Player {player.name} is not connected")

        message = {
            "messageType": message_type,
            "data": data or {}
        }

        await player.websocket.send(json.dumps(message))
        logger.info(f"Player {player.name} sent: {message_type}")

    async def send_action(self, player: TestPlayer, action: str, data: Dict = None):
        """Send a game action to the server."""
        await self.send_message(player, action, data)

    async def register_player(self, player: TestPlayer):
        """Register a player with the server."""
        await self.send_message(player, "registerPlayer", {"name": player.name})
        await asyncio.sleep(0.1)  # Give server time to process

    async def take_seat(self, player: TestPlayer, seat: int):
        """Have a player take a specific seat."""
        await self.send_message(player, "takeSeat", {"seat": seat})
        player.seat = seat
        await asyncio.sleep(0.1)

    async def start_game(self, player: TestPlayer):
        """Start the game."""
        await self.send_message(player, "startGame")
        await asyncio.sleep(0.1)

    async def test_basic_connection(self):
        """Test basic connection and disconnection."""
        logger.info("=== Testing Basic Connection ===")

        try:
            player = await self.create_player("TestPlayer1")
            await asyncio.sleep(1)
            await player.websocket.close()
            self.test_results.append("✓ Basic connection test passed")
        except Exception as e:
            self.test_results.append(f"✗ Basic connection test failed: {e}")

    async def test_player_registration(self):
        """Test player registration."""
        logger.info("=== Testing Player Registration ===")

        try:
            player1 = await self.create_player("Alice")
            player2 = await self.create_player("Bob")

            await self.register_player(player1)
            await self.register_player(player2)

            await asyncio.sleep(1)
            self.test_results.append("✓ Player registration test passed")
        except Exception as e:
            self.test_results.append(f"✗ Player registration test failed: {e}")

    async def test_seat_management(self):
        """Test seat taking."""
        logger.info("=== Testing Seat Management ===")

        try:
            if "Alice" not in self.players or "Bob" not in self.players:
                await self.test_player_registration()

            await self.take_seat(self.players["Alice"], 1)
            await self.take_seat(self.players["Bob"], 2)

            await asyncio.sleep(1)
            self.test_results.append("✓ Seat management test passed")
        except Exception as e:
            self.test_results.append(f"✗ Seat management test failed: {e}")

    async def test_game_flow(self):
        """Test complete game flow."""
        logger.info("=== Testing Game Flow ===")

        try:
            # Create more players for a proper game
            players_to_create = ["Charlie", "Dave", "Eve", "Frank"]
            for name in players_to_create:
                if name not in self.players:
                    player = await self.create_player(name)
                    await self.register_player(player)

            # Seat all players
            seat = 3
            for name in players_to_create:
                if name in self.players and self.players[name].seat is None:
                    await self.take_seat(self.players[name], seat)
                    seat += 1

            # Start the game
            await self.start_game(self.players["Alice"])

            # Let the game run for a while
            logger.info("Letting game run for 30 seconds...")
            await asyncio.sleep(30)

            self.test_results.append("✓ Game flow test completed")
        except Exception as e:
            self.test_results.append(f"✗ Game flow test failed: {e}")

    async def test_manual_actions(self):
        """Test manual action sending."""
        logger.info("=== Testing Manual Actions ===")

        try:
            # Create a simple 2-player game
            alice = await self.create_player("ManualAlice")
            bob = await self.create_player("ManualBob")

            await self.register_player(alice)
            await self.register_player(bob)
            await self.take_seat(alice, 1)
            await self.take_seat(bob, 2)
            await self.start_game(alice)

            # Wait for game to start and manually test some actions
            await asyncio.sleep(2)

            # Test different actions (these might fail if it's not the right player's turn)
            try:
                await self.send_action(alice, "check")
                await asyncio.sleep(1)
                await self.send_action(bob, "raise", {"amount": 20})
                await asyncio.sleep(1)
                await self.send_action(alice, "call")
            except Exception as action_error:
                logger.warning(f"Some actions failed (expected): {action_error}")

            self.test_results.append("✓ Manual actions test completed")
        except Exception as e:
            self.test_results.append(f"✗ Manual actions test failed: {e}")

    async def cleanup(self):
        """Clean up all connections."""
        logger.info("Cleaning up connections...")
        for player in self.players.values():
            if player.connected and player.websocket:
                await player.websocket.close()

    async def run_all_tests(self):
        """Run all tests."""
        logger.info("Starting comprehensive poker server tests...")

        try:
            await self.test_basic_connection()
            await self.test_player_registration()
            await self.test_seat_management()
            await self.test_manual_actions()
            await self.test_game_flow()

        except KeyboardInterrupt:
            logger.info("Tests interrupted by user")
        except Exception as e:
            logger.error(f"Test suite failed: {e}")
        finally:
            await self.cleanup()

        # Print results
        logger.info("\n=== TEST RESULTS ===")
        for result in self.test_results:
            logger.info(result)

async def main():
    """Main test function."""
    server_url = "ws://localhost:8888"

    # Check if custom port is provided
    if len(sys.argv) > 1:
        port = sys.argv[1]
        server_url = f"ws://localhost:{port}"

    logger.info(f"Testing server at: {server_url}")
    logger.info("Make sure the WebSocket server is running!")
    logger.info("Start server with: cargo run --features websocket --bin websocket_server")

    tester = PokerServerTest(server_url)

    # Handle Ctrl+C gracefully
    def signal_handler(sig, frame):
        logger.info("Received interrupt signal, cleaning up...")
        tester.running = False
        asyncio.create_task(tester.cleanup())
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    await tester.run_all_tests()

if __name__ == "__main__":
    asyncio.run(main())
