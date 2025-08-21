// Test the simplified rank_hand function
use pokers::state::card::{Card, CardRank, CardSuit};

// Define the necessary external functions for testing
extern "C" {
    fn evaluate_5cards(a: i32, b: i32, c: i32, d: i32, e: i32) -> i32;
    fn evaluate_7cards(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32) -> i32;
}

fn card_to_phevaluator_int(card: Card) -> i32 {
    let rank_value = match card.rank {
        CardRank::R2 => 0,
        CardRank::R3 => 1,
        CardRank::R4 => 2,
        CardRank::R5 => 3,
        CardRank::R6 => 4,
        CardRank::R7 => 5,
        CardRank::R8 => 6,
        CardRank::R9 => 7,
        CardRank::RT => 8,
        CardRank::RJ => 9,
        CardRank::RQ => 10,
        CardRank::RK => 11,
        CardRank::RA => 12,
    };

    let suit_value = match card.suit {
        CardSuit::Clubs => 0,
        CardSuit::Diamonds => 1,
        CardSuit::Hearts => 2,
        CardSuit::Spades => 3,
    };

    rank_value * 4 + suit_value
}

fn test_rank_hand_simplified(private_cards: (Card, Card), public_cards: &Vec<Card>) -> u64 {
    let mut cards = public_cards.clone();
    cards.append(&mut vec![private_cards.0, private_cards.1]);

    // Check if we have enough cards for a valid combination
    if cards.len() < 5 {
        return 7463; // Worse than phevaluator's worst rank (7462)
    }

    // If we have exactly 7 cards, use phevaluator directly
    if cards.len() == 7 {
        let card_ints: Vec<i32> = cards.iter().map(|c| card_to_phevaluator_int(*c)).collect();
        let rank = unsafe {
            evaluate_7cards(
                card_ints[0],
                card_ints[1],
                card_ints[2],
                card_ints[3],
                card_ints[4],
                card_ints[5],
                card_ints[6],
            )
        };

        return rank as u64;
    }

    // For 5 or 6 cards, find the best 5-card combination
    if cards.len() >= 5 {
        use itertools::Itertools;
        let min_rank = cards
            .iter()
            .copied()
            .combinations(5)
            .map(|comb| {
                let card_ints: Vec<i32> =
                    comb.iter().map(|c| card_to_phevaluator_int(*c)).collect();
                unsafe {
                    evaluate_5cards(
                        card_ints[0],
                        card_ints[1],
                        card_ints[2],
                        card_ints[3],
                        card_ints[4],
                    ) as u64
                }
            })
            .min()
            .unwrap_or(7463);

        return min_rank;
    }

    7463
}

fn main() {
    println!("Testing simplified rank_hand function...");

    // Test 1: Royal Flush (should be rank 1)
    let royal_flush_cards = vec![
        Card::new(CardSuit::Spades, CardRank::RT), // 10 of Spades
        Card::new(CardSuit::Spades, CardRank::RJ), // Jack of Spades
        Card::new(CardSuit::Spades, CardRank::RQ), // Queen of Spades
        Card::new(CardSuit::Spades, CardRank::RK), // King of Spades
        Card::new(CardSuit::Spades, CardRank::RA), // Ace of Spades
    ];

    let hole_cards = (
        Card::new(CardSuit::Hearts, CardRank::R2), // 2 of Hearts (irrelevant)
        Card::new(CardSuit::Clubs, CardRank::R3),  // 3 of Clubs (irrelevant)
    );

    let royal_rank = test_rank_hand_simplified(hole_cards, &royal_flush_cards);
    println!("Royal Flush rank: {} (expected: 1)", royal_rank);

    // Test 2: Pair of Aces
    let pair_cards = vec![
        Card::new(CardSuit::Spades, CardRank::RA), // Ace of Spades
        Card::new(CardSuit::Hearts, CardRank::RA), // Ace of Hearts
        Card::new(CardSuit::Diamonds, CardRank::R2), // 2 of Diamonds
        Card::new(CardSuit::Clubs, CardRank::R3),  // 3 of Clubs
        Card::new(CardSuit::Diamonds, CardRank::R4), // 4 of Diamonds
    ];

    let hole_cards2 = (
        Card::new(CardSuit::Hearts, CardRank::R5), // 5 of Hearts (irrelevant)
        Card::new(CardSuit::Clubs, CardRank::R6),  // 6 of Clubs (irrelevant)
    );

    let pair_rank = test_rank_hand_simplified(hole_cards2, &pair_cards);
    println!(
        "Pair of Aces rank: {} (should be much better than high card)",
        pair_rank
    );

    // Test 3: High card
    let high_card_cards = vec![
        Card::new(CardSuit::Spades, CardRank::RA), // Ace of Spades
        Card::new(CardSuit::Hearts, CardRank::RK), // King of Hearts
        Card::new(CardSuit::Diamonds, CardRank::R2), // 2 of Diamonds
        Card::new(CardSuit::Clubs, CardRank::R4),  // 4 of Clubs
        Card::new(CardSuit::Diamonds, CardRank::R6), // 6 of Diamonds
    ];

    let hole_cards3 = (
        Card::new(CardSuit::Hearts, CardRank::R8), // 8 of Hearts
        Card::new(CardSuit::Clubs, CardRank::RT),  // 10 of Clubs
    );

    let high_card_rank = test_rank_hand_simplified(hole_cards3, &high_card_cards);
    println!(
        "High card rank: {} (should be close to 7462)",
        high_card_rank
    );

    // Verify ranking order (lower rank = better hand)
    if royal_rank < pair_rank && pair_rank < high_card_rank {
        println!("✓ Ranking order is correct: Royal Flush < Pair < High Card");
    } else {
        println!("✗ Ranking order is incorrect!");
    }

    if royal_rank == 1 {
        println!("✓ Royal Flush correctly identified as rank 1");
    } else {
        println!("✗ Royal Flush should be rank 1, got {}", royal_rank);
    }

    println!("Test completed!");
}
