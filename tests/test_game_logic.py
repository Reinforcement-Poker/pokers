import pytest
import pokers as pkrs
import numpy as np
import json


def test_game_logic_against_pluribus_logs():
    with open("tests/test_files/pluribus_logs.json") as f:
        pluribus_data = json.load(f)

    for i, pb_hand in enumerate(pluribus_data):
        n_players = len(pb_hand["players"])
        button = pb_hand["button"]
        str_deck = [c for hand in pb_hand["private_cards"] for c in hand]
        if "public_cards" in pb_hand:
            if "flop" in pb_hand["public_cards"]:
                str_deck += pb_hand["public_cards"]["flop"]
            if "turn" in pb_hand["public_cards"]:
                str_deck += [pb_hand["public_cards"]["turn"]]
            if "river" in pb_hand["public_cards"]:
                str_deck += [pb_hand["public_cards"]["river"]]
        deck = []
        for str_c in str_deck:
            c = pkrs.Card.from_string(str_c[::-1])
            assert c is not None
            deck.append(c)

        pkrs_state = pkrs.State.from_deck(
            n_players=n_players, button=button, deck=deck, sb=50, bb=100, stake=np.inf
        )
        print(f"|{i}> game: {pb_hand['game']}, index: {pb_hand['index']}")
        print(pkrs.visualize_trace([pkrs_state]))
        prev_state_final = False
        for pb_stage, pb_actions in pb_hand["actions"].items():
            for pb_action in pb_actions:
                print(pb_action)
                assert not prev_state_final
                assert pkrs_state.status == pkrs.StateStatus.Ok
                assert pkrs_state.stage == pkrs.Stage.__dict__[pb_stage.capitalize()]
                assert pkrs_state.current_player == pb_action["player"]
                amount = pb_action.get("amount", 0)
                action = pkrs.Action(
                    pkrs.ActionEnum.__dict__[pb_action["action"].capitalize()], amount
                )
                prev_state_final = pkrs_state.final_state
                pkrs_state = pkrs_state.apply_action(action)
                print(pkrs.visualize_state(pkrs_state))

        assert pkrs_state.final_state
        print("Pkrs rewards:", [ps.reward for ps in pkrs_state.players_state])
        print("Real rewards:", pb_hand["rewards"])
        for p, r in enumerate(pb_hand["rewards"]):
            assert pkrs_state.players_state[p].reward == r
