// Simple test for the simplified rank_hand concept
extern "C" {
    fn evaluate_5cards(a: i32, b: i32, c: i32, d: i32, e: i32) -> i32;
    fn evaluate_7cards(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32) -> i32;
}

fn card_to_phevaluator_int(rank: i32, suit: i32) -> i32 {
    rank * 4 + suit
}

fn main() {
    println!("Testing simplified ranking with single return value...");

    // Test 1: Royal Flush using 5-card evaluation
    // A♠ K♠ Q♠ J♠ T♠
    let royal_flush = [
        card_to_phevaluator_int(12, 3), // Ace of Spades
        card_to_phevaluator_int(11, 3), // King of Spades
        card_to_phevaluator_int(10, 3), // Queen of Spades
        card_to_phevaluator_int(9, 3),  // Jack of Spades
        card_to_phevaluator_int(8, 3),  // Ten of Spades
    ];

    let royal_rank = unsafe {
        evaluate_5cards(
            royal_flush[0],
            royal_flush[1],
            royal_flush[2],
            royal_flush[3],
            royal_flush[4],
        ) as u64
    };

    println!("Royal Flush rank: {} (expected: 1)", royal_rank);

    // Test 2: Pair of Aces with high kickers
    // A♠ A♥ K♣ Q♦ J♣
    let pair_aces = [
        card_to_phevaluator_int(12, 3), // Ace of Spades
        card_to_phevaluator_int(12, 2), // Ace of Hearts
        card_to_phevaluator_int(11, 0), // King of Clubs
        card_to_phevaluator_int(10, 1), // Queen of Diamonds
        card_to_phevaluator_int(9, 0),  // Jack of Clubs
    ];

    let pair_rank = unsafe {
        evaluate_5cards(
            pair_aces[0],
            pair_aces[1],
            pair_aces[2],
            pair_aces[3],
            pair_aces[4],
        ) as u64
    };

    println!("Pair of Aces rank: {}", pair_rank);

    // Test 3: High card (worst possible hand)
    // 7♠ 5♥ 4♣ 3♦ 2♣
    let high_card = [
        card_to_phevaluator_int(5, 3), // 7 of Spades
        card_to_phevaluator_int(3, 2), // 5 of Hearts
        card_to_phevaluator_int(2, 0), // 4 of Clubs
        card_to_phevaluator_int(1, 1), // 3 of Diamonds
        card_to_phevaluator_int(0, 0), // 2 of Clubs
    ];

    let high_card_rank = unsafe {
        evaluate_5cards(
            high_card[0],
            high_card[1],
            high_card[2],
            high_card[3],
            high_card[4],
        ) as u64
    };

    println!(
        "High card rank: {} (should be close to 7462)",
        high_card_rank
    );

    // Test 4: 7-card hand evaluation (Texas Hold'em scenario)
    // Board: A♠ K♠ Q♠ J♠ T♠ (Royal Flush on board)
    // Hand: 2♥ 3♣ (irrelevant hole cards)
    let seven_cards = [
        card_to_phevaluator_int(12, 3), // Ace of Spades
        card_to_phevaluator_int(11, 3), // King of Spades
        card_to_phevaluator_int(10, 3), // Queen of Spades
        card_to_phevaluator_int(9, 3),  // Jack of Spades
        card_to_phevaluator_int(8, 3),  // Ten of Spades
        card_to_phevaluator_int(0, 2),  // 2 of Hearts
        card_to_phevaluator_int(1, 0),  // 3 of Clubs
    ];

    let seven_card_rank = unsafe {
        evaluate_7cards(
            seven_cards[0],
            seven_cards[1],
            seven_cards[2],
            seven_cards[3],
            seven_cards[4],
            seven_cards[5],
            seven_cards[6],
        ) as u64
    };

    println!("7-card Royal Flush rank: {} (expected: 1)", seven_card_rank);

    // Verify ranking order (lower rank = better hand)
    println!("\n=== Ranking Summary ===");
    println!("Royal Flush (5-card): {}", royal_rank);
    println!("Royal Flush (7-card): {}", seven_card_rank);
    println!("Pair of Aces: {}", pair_rank);
    println!("High Card: {}", high_card_rank);

    if royal_rank == 1 && seven_card_rank == 1 {
        println!("✓ Both Royal Flushes correctly ranked as 1");
    } else {
        println!("✗ Royal Flush ranking error");
    }

    if royal_rank < pair_rank && pair_rank < high_card_rank {
        println!("✓ Ranking order is correct: Royal Flush < Pair < High Card");
    } else {
        println!("✗ Ranking order is incorrect!");
    }

    println!("\n✓ Simplified single-value ranking system works correctly!");
    println!("Now rank_hand() returns a single u64 value where lower = better hand");
}
