#!/usr/bin/env python3

import sys
import os

# Add the target directory to the path so we can import the compiled library
sys.path.insert(0, 'target/debug')

try:
    import pokers
    from pokers import State, Card, CardSuit, CardRank

    print("Testing PokerHandEvaluator integration through Python interface...")

    # Create a test game state
    state = State.from_seed(n_players=2, button=0, sb=1.0, bb=2.0, stake=100.0, seed=12345, verbose=True)
    print(f"Created initial state: {state.stage}")

    # Let's simulate a few actions and see if hand evaluation works
    print("Applying some actions...")

    # Player 0 calls
    from pokers import Action, ActionEnum
    call_action = Action(ActionEnum.CheckCall, 0.0)
    state = state.apply_action(call_action)
    print(f"After first action, stage: {state.stage}")

    # Player 1 checks
    check_action = Action(ActionEnum.CheckCall, 0.0)
    state = state.apply_action(check_action)
    print(f"After second action, stage: {state.stage}")

    # Continue until we have community cards
    while len(state.public_cards) == 0 and not state.final_state:
        legal_actions = state.legal_actions
        if legal_actions:
            # Take the first legal action
            action = Action(legal_actions[0], 0.0)
            state = state.apply_action(action)
            print(f"Applied action, stage: {state.stage}, public cards: {len(state.public_cards)}")
        else:
            break

    print(f"Final state: stage={state.stage}, public_cards={len(state.public_cards)}, final={state.final_state}")

    if state.public_cards:
        print("Public cards:")
        for card in state.public_cards:
            print(f"  {card}")

    if state.final_state:
        print("Game completed successfully!")
        for i, player in enumerate(state.players_state):
            print(f"Player {i}: reward={player.reward}, active={player.active}")

    print("✓ PokerHandEvaluator integration test passed!")

except ImportError as e:
    print(f"Failed to import pokers module: {e}")
    print("Make sure to run 'cargo build' first")
    sys.exit(1)
except Exception as e:
    print(f"Test failed with error: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)
