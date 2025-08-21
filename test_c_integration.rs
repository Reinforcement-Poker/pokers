// Simple test to verify PokerHandEvaluator linking
extern "C" {
    fn evaluate_7cards(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32) -> i32;
}

fn card_to_phevaluator_int(rank: i32, suit: i32) -> i32 {
    rank * 4 + suit
}

fn main() {
    println!("Testing PokerHandEvaluator C library integration...");

    // Create 7 cards for testing: A♠ K♠ Q♠ J♠ T♠ 2♥ 3♣ (Royal Flush + 2 random cards)
    let cards = [
        card_to_phevaluator_int(12, 3), // Ace of Spades (rank=12, suit=3)
        card_to_phevaluator_int(11, 3), // King of Spades
        card_to_phevaluator_int(10, 3), // Queen of Spades
        card_to_phevaluator_int(9, 3),  // Jack of Spades
        card_to_phevaluator_int(8, 3),  // Ten of Spades
        card_to_phevaluator_int(0, 2),  // 2 of Hearts
        card_to_phevaluator_int(1, 0),  // 3 of Clubs
    ];

    // Call the C function
    let rank = unsafe {
        evaluate_7cards(
            cards[0], cards[1], cards[2], cards[3], cards[4], cards[5], cards[6],
        )
    };

    println!("Hand rank: {} (1 is best, 7462 is worst)", rank);
    println!("Expected: 1 (Royal Flush should be the best hand)");

    if rank == 1 {
        println!("✓ Test PASSED! Got expected rank for Royal Flush");
    } else {
        println!("✗ Test FAILED! Expected rank 1, got {}", rank);
    }

    // Test another hand: pair of Aces
    let pair_cards = [
        card_to_phevaluator_int(12, 3), // Ace of Spades
        card_to_phevaluator_int(12, 2), // Ace of Hearts
        card_to_phevaluator_int(0, 1),  // 2 of Diamonds
        card_to_phevaluator_int(1, 0),  // 3 of Clubs
        card_to_phevaluator_int(2, 1),  // 4 of Diamonds
        card_to_phevaluator_int(3, 2),  // 5 of Hearts
        card_to_phevaluator_int(4, 3),  // 6 of Spades
    ];

    let pair_rank = unsafe {
        evaluate_7cards(
            pair_cards[0],
            pair_cards[1],
            pair_cards[2],
            pair_cards[3],
            pair_cards[4],
            pair_cards[5],
            pair_cards[6],
        )
    };

    println!("Pair of Aces rank: {}", pair_rank);

    if pair_rank > 1 && pair_rank <= 7462 {
        println!(
            "✓ Pair ranking test PASSED! Got reasonable rank: {}",
            pair_rank
        );
    } else {
        println!(
            "✗ Pair ranking test FAILED! Got unexpected rank: {}",
            pair_rank
        );
    }

    println!("PokerHandEvaluator C library integration test completed!");
}
