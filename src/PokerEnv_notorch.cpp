#include "PokerEnv_notorch.h"
#include <iostream>
#include <sstream> // For std::stringstream in toString()
#include <numeric>
#include <random>
#include <algorithm>
#include <set> // For std::set in reset()
#include <limits> // For std::numeric_limits
#include <cmath> // For std::abs
#include <fstream> // For std::ifstream
#include <phevaluator/phevaluator.h> // For hand evaluation
#include "../../PokerHandEvaluator/cpp/include/phevaluator/evaluator_holdem_potential.h"

// Forward declaration for evaluate_2cards function from evaluator_extended.c
extern "C" {
    int evaluate_2cards(int a, int b);
}

// Define constants if not already in PokerEnv_notorch.h (they should be)
#ifndef PREFLOP
#define PREFLOP 0
#define FLOP 1
#define TURN 2
#define RIVER 3
#endif

#ifndef FOLD
#define FOLD 0
#define CHECK_CALL 1
#define BET_RAISE 2
#endif

// Forward declarations for helper functions that were in anonymous namespace
static void printStringAsBytes(const std::string& s, const std::string& label = "");
static std::tuple<bool, Card::Suit, size_t> findSuitInString(const std::string& cardStr);
static std::pair<Card::CardValue, Card::Suit> parseCardStringInternal(const std::string& cardStr);
static std::string getCardString(Card::CardValue value, Card::Suit suit);


// Helper function implementations (moved from anonymous namespace)
static void printStringAsBytes(const std::string& s, const std::string& label) {
    if (!label.empty()) {
        std::cout << label << ": \\\"";
    }
    for (char c : s) {
        std::cout << std::hex << static_cast<int>(static_cast<unsigned char>(c)) << " ";
    }
    if (!label.empty()) {
        std::cout << "\\\" (length: " << std::dec << s.length() << ")" << std::endl;
    }
}

static std::tuple<bool, Card::Suit, size_t> findSuitInString(const std::string& cardStr) {
    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG findSuitInString] Input cardStr: ";
    printStringAsBytes(cardStr);
    #endif

    // IMPORTANT: Ensure this file (PokerEnv_notorch.cpp) is saved with UTF-8 encoding
    const std::string spade_unicode = "♠"; // UTF-8: e2 99 a0
    const std::string heart_unicode = "♥"; // UTF-8: e2 99 a5 (Note: original log showed e2 99 a1 for ♡, heart suit in Card.cpp is \u2661 which is ♡ not ♥)
                                        // Let's stick to the symbols from Card.cpp: ♢, ♣, ♡, ♠
    const std::string diamond_unicode = "♢"; // Card::SuitString[0] = \\u2662
    const std::string club_unicode = "♣";    // Card::SuitString[1] = \\u2663
    const std::string heart_alt_unicode = "♡"; // Card::SuitString[2] = \\u2661
    // spade_unicode is already "♠" (Card::SuitString[3] = \\u2660)


    #ifdef DEBUG_POKER_ENV
    printStringAsBytes(spade_unicode, "[DEBUG findSuitInString] spade_unicode ('♠')");
    printStringAsBytes(heart_alt_unicode, "[DEBUG findSuitInString] heart_alt_unicode ('♡')");
    printStringAsBytes(club_unicode, "[DEBUG findSuitInString] club_unicode ('♣')");
    printStringAsBytes(diamond_unicode, "[DEBUG findSuitInString] diamond_unicode ('♢')");
    #endif

    // Check for multi-byte Unicode suits first
    if (cardStr.length() >= spade_unicode.length() && cardStr.rfind(spade_unicode) == cardStr.length() - spade_unicode.length()) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG findSuitInString] Matched spade_unicode" << std::endl;
        #endif
        return {true, Card::Spades, spade_unicode.length()};
    }
    if (cardStr.length() >= heart_alt_unicode.length() && cardStr.rfind(heart_alt_unicode) == cardStr.length() - heart_alt_unicode.length()) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG findSuitInString] Matched heart_alt_unicode (♡)" << std::endl;
        #endif
        return {true, Card::Hearts, heart_alt_unicode.length()};
    }
    if (cardStr.length() >= club_unicode.length() && cardStr.rfind(club_unicode) == cardStr.length() - club_unicode.length()) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG findSuitInString] Matched club_unicode" << std::endl;
        #endif
        return {true, Card::Clubs, club_unicode.length()};
    }
    if (cardStr.length() >= diamond_unicode.length() && cardStr.rfind(diamond_unicode) == cardStr.length() - diamond_unicode.length()) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG findSuitInString] Matched diamond_unicode" << std::endl;
        #endif
        return {true, Card::Diamonds, diamond_unicode.length()};
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG findSuitInString] No Unicode suit matched. Trying ASCII." << std::endl;
    #endif

    // If no Unicode suit found, check for single ASCII character suits
    if (cardStr.length() >= 1) {
        char lastChar = cardStr.back();
        if (lastChar == 's' || lastChar == 'S') return {true, Card::Spades, 1};
        if (lastChar == 'h' || lastChar == 'H') return {true, Card::Hearts, 1};
        if (lastChar == 'c' || lastChar == 'C') return {true, Card::Clubs, 1};
        if (lastChar == 'd' || lastChar == 'D') return {true, Card::Diamonds, 1};
    }
    return {false, Card::Diamonds, 0}; // Default if not found
}

static std::pair<Card::CardValue, Card::Suit> parseCardStringInternal(const std::string& cardStr) {
    if (cardStr.empty()) {
        throw std::invalid_argument("Card string cannot be empty");
    }

    bool suitFound_flag;
    Card::Suit suit_enum_val;
    size_t suit_len_in_str_val;

    std::tie(suitFound_flag, suit_enum_val, suit_len_in_str_val) = findSuitInString(cardStr);

    if (!suitFound_flag) {
        throw std::invalid_argument("Invalid suit in card string: \\" + cardStr + "\\\"");
    }

    if (cardStr.length() <= suit_len_in_str_val) {
         throw std::invalid_argument("Card string is too short (likely only a suit): \\" + cardStr + "\\\"");
    }
    std::string valueStr = cardStr.substr(0, cardStr.length() - suit_len_in_str_val);
    if (valueStr.empty()) {
         throw std::invalid_argument("Value part of card string is empty after suit extraction: \\" + cardStr + "\\\"");
    }

    Card::CardValue value_enum_val = Card::Two; // Default
    bool valueFound_flag = false;

    // Convert valueStr to uppercase for case-insensitive comparison for single char values
    std::string valueStrUpper = valueStr;
    if (valueStrUpper.length() == 1) {
        valueStrUpper[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(valueStrUpper[0])));
    }
    // For "10", it remains "10"

    for (int v_idx = 0; v_idx < CARDVALUESIZE; ++v_idx) {
        // Compare with stored Card::ValueString (which are uppercase e.g., "A", "K", "10")
        if (Card::ValueString[v_idx] == valueStrUpper) {
            value_enum_val = static_cast<Card::CardValue>(v_idx);
            valueFound_flag = true;
            break;
        }
        // Special handling for 'T' vs "10"
        if (Card::ValueString[v_idx] == "10" && valueStrUpper == "T") {
            value_enum_val = static_cast<Card::CardValue>(v_idx); // This should map to Card::Ten
            valueFound_flag = true;
            break;
        }
    }

    if (!valueFound_flag) {
        throw std::invalid_argument("Invalid value string: \\" + valueStr + "\\\" in card \\" + cardStr + "\\\"");
    }
    return {value_enum_val, suit_enum_val};
}

static std::string getCardString(Card::CardValue value, Card::Suit suit) {
    return Card::ValueString[static_cast<int>(value)] + Card::SuitStringASCII[static_cast<int>(suit)];
}

// Cache for canonical suit map results. Key is board long, value is the map.
static std::map<int64_t, std::vector<int>> suit_map_cache;

// ================================
// PokerEnv Implementation
// ================================

PokerEnv::PokerEnv(const nlohmann::json& config,
                  int nSeats, const std::vector<float>& bet_sizes_as_frac_of_pot, bool uniform_action_interpolation,
                  int smallBlind, int bigBlind, int ante, int defaultStackSize)
    : args_config(config), // Store the configuration
      SMALL_BLIND(smallBlind),
      BIG_BLIND(bigBlind),
      ANTE(ante),
      DEFAULT_STACK_SIZE(defaultStackSize),
      betSizesListAsFracOfPot(bet_sizes_as_frac_of_pot),
      uniformActionInterpolation_member(uniform_action_interpolation),
      N_SEATS(nSeats),
      m_rng(std::random_device{}())
{

    // 安全地读取配置，处理空配置的情况
    if (args_config.contains("mode_settings") && args_config["mode_settings"].is_object()) {
        IS_EVALUATING = args_config["mode_settings"].value("is_evaluating", false);
    } else {
        IS_EVALUATING = false;
    }

    // 读取观察模式配置
    // 首先从顶层读取debug_obs_flag（用于Python绑定兼容性）
    debug_obs_flag = args_config.value("debug_obs_flag", false);
    #ifdef DEBUG_POKER_ENV
    std::cout << "PokerEnv constructor: debug_obs_flag=" << debug_obs_flag << std::endl;
#endif
    if (args_config.contains("game_settings") && args_config["game_settings"].is_object()) {
        const auto& game_settings = args_config["game_settings"];
        use_simplified_observation = game_settings.value("use_simplified_observation", false);
        // 如果game_settings中也有debug_obs_flag，则优先使用
        debug_obs_flag = game_settings.value("debug_obs_flag", debug_obs_flag);
        FIRST_ACTION_NO_CALL = game_settings.value("first_action_no_call", false);
        IS_FIXED_LIMIT_GAME = game_settings.value("is_fixed_limit_game", false);

    } else {
        // 当没有配置文件时，尝试从默认配置文件读取
        std::ifstream config_file("src/poker_config.json");
        if (!config_file.is_open()) {
            // 尝试其他可能的路径
            config_file.open("../backend/src/poker_config.json");
        }
        if (!config_file.is_open()) {
            config_file.open("backend/src/poker_config.json");
        }
        if (config_file.is_open()) {
            nlohmann::json default_config;
            config_file >> default_config;
            if (default_config.contains("game_settings") && default_config["game_settings"].is_object()) {
                const auto& game_settings = default_config["game_settings"];
                use_simplified_observation = game_settings.value("use_simplified_observation", false);
                debug_obs_flag = game_settings.value("debug_obs_flag", false);
                FIRST_ACTION_NO_CALL = game_settings.value("first_action_no_call", false);
                IS_FIXED_LIMIT_GAME = game_settings.value("is_fixed_limit_game", false);
            } else {
                use_simplified_observation = false;
                // 不要重置debug_obs_flag，保持之前从args_config读取的值
                FIRST_ACTION_NO_CALL = false;
                IS_FIXED_LIMIT_GAME = false;
            }
            if (default_config.contains("mode_settings") && default_config["mode_settings"].is_object()) {
                IS_EVALUATING = default_config["mode_settings"].value("is_evaluating", false);
            }

        } else {
            use_simplified_observation = false;
            FIRST_ACTION_NO_CALL = false;
            IS_FIXED_LIMIT_GAME = false;
        }
    }

    bool scaleRewards = false;

    if (config.contains("reward_settings") && config["reward_settings"].is_object() &&
        config["reward_settings"].contains("scale_rewards") && config["reward_settings"]["scale_rewards"].is_boolean()) {
        scaleRewards = config["reward_settings"]["scale_rewards"].get<bool>();
    }

    if (config.contains("game_settings") && config["game_settings"].is_object() &&
        config["game_settings"].contains("starting_stack_sizes_list") && config["game_settings"]["starting_stack_sizes_list"].is_array()) {
        for (const auto& stack_val : config["game_settings"]["starting_stack_sizes_list"]) {
            if (stack_val.is_number_integer()) {
                startingStackSizesList.push_back(stack_val.get<int>());
            }
        }
    }

    // REWARD_SCALAR will be calculated after players are initialized
    REWARD_SCALAR = 1.0f; // Temporary default value

    ROUND_BEFORE[PREFLOP] = PREFLOP;
    ROUND_BEFORE[FLOP] = PREFLOP;
    ROUND_BEFORE[TURN] = FLOP;
    ROUND_BEFORE[RIVER] = TURN;

    ROUND_AFTER[PREFLOP] = FLOP;
    ROUND_AFTER[FLOP] = TURN;
    ROUND_AFTER[TURN] = RIVER;
    ROUND_AFTER[RIVER] = -1; // No round after RIVER

    ALL_ROUNDS_LIST = {PREFLOP, FLOP, TURN, RIVER};
    N_ACTIONS = 2 + betSizesListAsFracOfPot.size();

    players.resize(N_SEATS);
    for (int i = 0; i < N_SEATS; ++i) {
        int stackSize = DEFAULT_STACK_SIZE;
         if (IS_EVALUATING && !startingStackSizesList.empty() && i < startingStackSizesList.size()){
            stackSize = startingStackSizesList[i];
        } else if (IS_EVALUATING) {
            std::uniform_int_distribution<int> stackDist(BIG_BLIND, DEFAULT_STACK_SIZE);
            stackSize = stackDist(m_rng);
        }
        players[i] = new PokerPlayer(i, stackSize);
    }

    // REWARD_SCALAR will be calculated after reset() is called and blinds are posted

    deck = std::make_unique<Deck>();

    communityCards.resize(N_COMMUNITY_CARDS, nullptr);

    sidePots.reserve(N_SEATS); // Max possible side pots

    // std::uniform_int_distribution<int> buttonDist(0, N_SEATS - 1);
    // buttonPos = buttonDist(m_rng);
    buttonPos = 4;
    // sbPos, bbPos, currentPlayer will be set in reset()

    // Initialize other members from PokerEnv.h if not done by default constructor
    lastAction_member = {-1, -1, -1};
    lastRaiser = -1;
    nRaisesThisRound = 0;
    nActionsThisEpisode = 0;
    cappedRaise_member.reset();

    FIRST_ACTION_NO_CALL = false;
    IS_FIXED_LIMIT_GAME = false;
    MAX_N_RAISES_PER_ROUND = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
    max_rounds_per_hand = 1000; // 默认值
    fix_utg_position = -1; // 默认不固定UTG位置
    end_with_round = 3; // 默认在RIVER轮次结束游戏

    // 只有当config不为空且包含所需字段时才处理
    if (!config.is_null() && config.is_object()) {
        if (config.contains("first_action_no_call")) {
            FIRST_ACTION_NO_CALL = config["first_action_no_call"].get<bool>();
        }
        if (config.contains("is_fixed_limit_game")) {
            IS_FIXED_LIMIT_GAME = config["is_fixed_limit_game"].get<bool>();
        }
        if (IS_FIXED_LIMIT_GAME && config.contains("max_n_raises_per_round")) {
            MAX_N_RAISES_PER_ROUND = config["max_n_raises_per_round"].get<std::vector<int>>();
        }
        // 从配置中读取max_rounds_per_hand
        if (config.contains("game_settings") && config["game_settings"].is_object() &&
            config["game_settings"].contains("max_rounds_per_hand") && config["game_settings"]["max_rounds_per_hand"].is_number_integer()) {
            max_rounds_per_hand = config["game_settings"]["max_rounds_per_hand"].get<int>();
        }
        // 从配置中读取fix_utg_position
        if (config.contains("game_settings") && config["game_settings"].is_object() &&
            config["game_settings"].contains("fix_utg_position") && config["game_settings"]["fix_utg_position"].is_number_integer()) {
            fix_utg_position = config["game_settings"]["fix_utg_position"].get<int>();
        }
        // 从配置中读取end_with_round
        if (config.contains("game_settings") && config["game_settings"].is_object() &&
            config["game_settings"].contains("end_with_round") && config["game_settings"]["end_with_round"].is_number_integer()) {
            end_with_round = config["game_settings"]["end_with_round"].get<int>();
            // 验证end_with_round的有效性
            if (end_with_round < 0 || end_with_round > 3) {
                end_with_round = 3; // 无效值时重置为默认值RIVER
            }
        }
    }
    _initPrivObsLookUp(); // Initialize LUT in constructor
    _initRangeIdxLut(); // Initialize range_idx LUT in constructor
    actions_this_street.resize(N_SEATS, 0); // 初始化actions_this_street

    bool custom_scenario = false;
    std::vector<std::vector<int>> hole_cards;
    std::vector<int> board_cards;

    if (config.contains("game_settings") && config["game_settings"].is_object()) {
        const auto& game_settings = config["game_settings"];
        if (game_settings.contains("hole_cards") && game_settings["hole_cards"].is_array() &&
            game_settings.contains("board_cards") && game_settings["board_cards"].is_array()) {

            for (const auto& player_hand_json : game_settings["hole_cards"]) {
                if (player_hand_json.is_array()) {
                    std::vector<int> hand;
                    for (const auto& card_val : player_hand_json) {
                        if (card_val.is_number_integer()) {
                            hand.push_back(card_val.get<int>());
                        }
                    }
                    hole_cards.push_back(hand);
                }
            }

            for (const auto& card_val : game_settings["board_cards"]) {
                if (card_val.is_number_integer()) {
                    board_cards.push_back(card_val.get<int>());
                }
            }

            if (!hole_cards.empty() || !board_cards.empty()) {
                custom_scenario = true;
            }
        }
    }

    // 初始化手牌强度缓存
    _initialHandStrengthCache.resize(N_SEATS, 0.0f);
    _handPotentialCache.resize(N_SEATS, {0, 0});

    // 初始化私有信息缓存
    cached_private_info.resize(N_SEATS);
    _currentPlayerInitialStrength = 0.0f;
    _currentPlayerHandPotential = 0.0f;

    if (custom_scenario) {

        reset(true, hole_cards, board_cards);
    } else {

        reset();
    }

}

PokerEnv::~PokerEnv() {
    for (PokerPlayer* p : players) {
        if (p) { // Check if player pointer is not null
            for (Card* c : p->hand) {
                delete c; // Delete Card objects PokerEnv allocated for player hands
            }
            // p->hand.clear(); // PokerPlayer's destructor will call clear()
            delete p;     // Delete the PokerPlayer object itself
        }
    }
    players.clear();

    // 清理 PokerEnv 拥有的 communityCards
    for (Card* c : communityCards) {
        delete c;
    }
    communityCards.clear();

    m_privObsLut.clear(); // Clear LUT in destructor
    m_rangeIdxLut.clear(); // Clear range_idx LUT in destructor
    m_idxToRangeLut.clear(); // Clear new reverse LUT in destructor
    // deck is std::unique_ptr, handles itself
}

void PokerEnv::_initPrivObsLookUp() {
    m_privObsLut.clear();
    const int N_RANKS_CONST = 13;
    const int N_SUITS_CONST = 4;
    const int N_CARDS_IN_DECK = N_RANKS_CONST * N_SUITS_CONST;
    const int N_HOLE_CARDS_CONST = 2;

    bool suits_matter = true;
    if (args_config.contains("game_settings") && args_config["game_settings"].is_object() &&
        args_config["game_settings"].contains("suits_matter") && args_config["game_settings"]["suits_matter"].is_boolean()) {
        suits_matter = args_config["game_settings"]["suits_matter"].get<bool>();
    }

    const int D_per_card = suits_matter ? (N_RANKS_CONST + N_SUITS_CONST) : N_RANKS_CONST;
    const int priv_obs_size = N_HOLE_CARDS_CONST * D_per_card;

    int current_idx = 0;
    for (int c1_1d = 0; c1_1d < N_CARDS_IN_DECK; ++c1_1d) {
        for (int c2_1d = c1_1d + 1; c2_1d < N_CARDS_IN_DECK; ++c2_1d) {
            int64_t range_idx = static_cast<int64_t>(current_idx);
            std::vector<float> priv_obs(priv_obs_size, 0.0f);

            int card1_value = c1_1d / N_SUITS_CONST;
            int card1_suit = c1_1d % N_SUITS_CONST;
            int card2_value = c2_1d / N_SUITS_CONST;
            int card2_suit = c2_1d % N_SUITS_CONST;

            // Card 1 encoding (smaller 1D index)
            int slot_offset = D_per_card * 0;
            if (card1_value >= 0 && card1_value < N_RANKS_CONST) {
                priv_obs[slot_offset + card1_value] = 1.0f;
            }
            if (suits_matter && card1_suit >= 0 && card1_suit < N_SUITS_CONST) {
                priv_obs[slot_offset + N_RANKS_CONST + card1_suit] = 1.0f;
            }

            // Card 2 encoding (larger 1D index)
            slot_offset = D_per_card * 1;
            if (card2_value >= 0 && card2_value < N_RANKS_CONST) {
                priv_obs[slot_offset + card2_value] = 1.0f;
            }
            if (suits_matter && card2_suit >= 0 && card2_suit < N_SUITS_CONST) {
                priv_obs[slot_offset + N_RANKS_CONST + card2_suit] = 1.0f;
            }

            m_privObsLut[range_idx] = priv_obs;
            current_idx++;
        }
    }
}

void PokerEnv::_initRangeIdxLut() {
    m_rangeIdxLut.clear();
    m_idxToRangeLut.clear(); // Clear the reverse LUT as well
    int current_idx = 0;
    for (int c1_1d = 0; c1_1d < 52; ++c1_1d) {
        for (int c2_1d = c1_1d + 1; c2_1d < 52; ++c2_1d) {
            m_rangeIdxLut[std::make_pair(c1_1d, c2_1d)] = static_cast<int64_t>(current_idx);
            m_idxToRangeLut[static_cast<int64_t>(current_idx)] = std::make_pair(c1_1d, c2_1d);
            current_idx++;
        }
    }
}


std::tuple<std::vector<std::vector<float>>, std::vector<float>> PokerEnv::reset() {
    std::fill(actions_this_street.begin(), actions_this_street.end(), 0); // 重置行动次数
    return reset(false); // Full reset
}

std::tuple<std::vector<std::vector<float>>, std::vector<float>> PokerEnv::reset(bool isNewRound) {
    std::fill(actions_this_street.begin(), actions_this_street.end(), 0); // 重置行动次数
    // 清空过往的观察值历史
    observationHistory.clear();
    // 重置历史动作记录
    actionHistory.clear();

    deck->shuffle();
    std::fill(communityCards.begin(), communityCards.end(), nullptr);

    if (!isNewRound) { // This is a full reset
        if (end_with_round == 0) {
            // When end_with_round is 0, disable random stacks and use starting_stack_sizes_list or 200*BB default.
            for (int i = 0; i < N_SEATS; ++i) {
                int stack_size;
                if (!startingStackSizesList.empty() && i < static_cast<int>(startingStackSizesList.size())) {
                    stack_size = startingStackSizesList[i];
                } else {
                    stack_size = 200 * BIG_BLIND;
                }
                players[i]->reset(isNewRound, stack_size); // isNewRound is false here
            }
        } else if (IS_EVALUATING) {
            // Original logic for evaluating mode (random stacks)
            std::set<int> usedStacks;
            std::uniform_int_distribution<int> stackDist(BIG_BLIND, DEFAULT_STACK_SIZE);
            for (int i = 0; i < N_SEATS; ++i) {
                int randomStack;
                if (!startingStackSizesList.empty() && i < static_cast<int>(startingStackSizesList.size())){
                     randomStack = startingStackSizesList[i];
                } else {
                    int attempts = 0;
                    const int maxAttempts = 1000;
                    do {
                        randomStack = stackDist(m_rng);
                        attempts++;
                    } while (usedStacks.count(randomStack) && attempts < maxAttempts);
                    if (usedStacks.count(randomStack)) { // still collision after attempts
                        while(usedStacks.count(randomStack)) randomStack++; // simple increment
                         if (randomStack > DEFAULT_STACK_SIZE) randomStack = BIG_BLIND + (randomStack - DEFAULT_STACK_SIZE -1) % (DEFAULT_STACK_SIZE - BIG_BLIND);
                    }
                }
                usedStacks.insert(randomStack);
                players[i]->reset(isNewRound, randomStack); // isNewRound is false here
            }
        } else { // Not end_with_round==0 and not IS_EVALUATING
            // Original default stack logic
            for (auto p : players) {
                p->reset(isNewRound); // isNewRound is false here, so resets to startingStack
            }
        }
    } else { // isNewRound is true
        // Original logic for preserving stacks
        for (auto p : players) {
            p->reset(isNewRound); // isNewRound is true here, so preserves stack
        }
    }

    _calculateRewardScalar();
    // _initPrivObsLookUp(); // Called in constructor and load_state_dict
    // 位置设置逻辑
    if (fix_utg_position >= 0 && fix_utg_position < N_SEATS) {
        // 固定UTG位置时，需要根据UTG反推按钮位置
        if (N_SEATS == 2) {
            // 两人桌：UTG就是SB/BTN，另一个是BB
            buttonPos = fix_utg_position;
            sbPos = buttonPos;
            bbPos = (buttonPos + 1) % N_SEATS;
        } else if (N_SEATS > 2) {
            // 多人桌：UTG是BB+1，所以BB位置是UTG-1，按钮位置是BB-2
            bbPos = (fix_utg_position - 1 + N_SEATS) % N_SEATS;
            sbPos = (bbPos - 1 + N_SEATS) % N_SEATS;
            buttonPos = (sbPos - 1 + N_SEATS) % N_SEATS;
        } else {
            // N_SEATS <= 1的特殊情况
            buttonPos = 0; sbPos = 0; bbPos = 0;
        }
    } else {
        // 使用默认位置逻辑
        if (isNewRound) {
            buttonPos = (buttonPos + 1) % N_SEATS;
        } else {
            std::uniform_int_distribution<int> buttonDist(0, N_SEATS - 1);
            buttonPos = buttonDist(m_rng);
        }

        if (N_SEATS == 2) {
            sbPos = buttonPos;
            bbPos = (buttonPos + 1) % N_SEATS;
        } else if (N_SEATS > 2) {
            sbPos = (buttonPos + 1) % N_SEATS;
            bbPos = (buttonPos + 2) % N_SEATS;
        } else { // N_SEATS <=1
            sbPos = 0; bbPos = 0;
        }
    }

    mainPot = 0;
    sidePots.clear();
    currentMainPot = 0; // 初始化实时主池
    currentSidePots.clear(); // 初始化实时边池
    currentSidePots.resize(N_SEATS, 0);
    lastRaiser = -1;
    nRaisesThisRound = 0;
    lastAction_member = {-1, -1, -1};
    nActionsThisEpisode = 0;
    cappedRaise_member.reset();
    handIsOver = false;

    currentRound = PREFLOP;

    _postAntes();
    _putCurrentBetsIntoMainPotAndSidePots();
    _postSmallBlind();
    _postBigBlind();

    _dealHoleCards();

    // 建立初始手牌强度缓存
    for (int i = 0; i < N_SEATS; ++i) {
        if (players[i] && players[i]->hand.size() == 2) {
            int handRank = getHandValuebyPlayer(i); // 1-169排名，1=最强，169=最弱
            // 转换为0-1范围的强度值：强度 = (170 - rank) / 169
            // AA(rank=1) -> strength=169/169=1.0, 72o(rank=169) -> strength=1/169≈0.006
            _initialHandStrengthCache[i] = static_cast<float>(170 - handRank) / 169.0f;
        } else {
            _initialHandStrengthCache[i] = 0.0f; // 无效手牌
        }
    }

    currentPlayer = _getFirstToActPreFlop();
    _updateHandPotentialForAllPlayers();

    // 计算并存储当前观察值到历史中
    std::vector<float> currentObservation = _calculateCurrentObservationByConfig();
    observationHistory.push_back(currentObservation);



    return getObservationForTransformer();
    // Calculate REWARD_SCALAR after players are initialized and blinds are posted
}



// 新增：可以设置手牌和公共牌的重载版本
std::tuple<std::vector<std::vector<float>>, std::vector<float>> PokerEnv::reset(bool isNewRound, const std::vector<std::vector<int>>& hole_cards, const std::vector<int>& board_cards) {
    std::fill(actions_this_street.begin(), actions_this_street.end(), 0); // 重置行动次数
    // 先进行标准重置
    observationHistory.clear();
    // 重置历史动作记录
    actionHistory.clear();

    // 如果有指定手牌或公共牌，创建新的干净的deck
    bool hasSpecifiedCards = !hole_cards.empty() || !board_cards.empty();
    if (hasSpecifiedCards) {
        // 创建新的完整deck
        deck = std::make_unique<Deck>();
        deck->shuffle();
    } else {
        // 没有指定卡牌时，使用现有deck并洗牌
        deck->shuffle();
    }

    std::fill(communityCards.begin(), communityCards.end(), nullptr);

    if (!isNewRound) { // This is a full reset
        if (end_with_round == 0) {
            // When end_with_round is 0, disable random stacks and use starting_stack_sizes_list or 200*BB default.
            for (int i = 0; i < N_SEATS; ++i) {
                int stack_size;
                if (!startingStackSizesList.empty() && i < static_cast<int>(startingStackSizesList.size())) {
                    stack_size = startingStackSizesList[i];
                } else {
                    stack_size = 200 * BIG_BLIND;
                }
                players[i]->reset(isNewRound, stack_size); // isNewRound is false here
            }
        } else if (IS_EVALUATING) {
            // Original logic for evaluating mode (random stacks)
            std::set<int> usedStacks;
            std::uniform_int_distribution<int> stackDist(BIG_BLIND, DEFAULT_STACK_SIZE);
            for (int i = 0; i < N_SEATS; ++i) {
                int randomStack;
                if (!startingStackSizesList.empty() && i < static_cast<int>(startingStackSizesList.size())){
                     randomStack = startingStackSizesList[i];
                } else {
                    int attempts = 0;
                    const int maxAttempts = 1000;
                    do {
                        randomStack = stackDist(m_rng);
                        attempts++;
                    } while (usedStacks.count(randomStack) && attempts < maxAttempts);
                    if (usedStacks.count(randomStack)) { // still collision after attempts
                        while(usedStacks.count(randomStack)) randomStack++; // simple increment
                         if (randomStack > DEFAULT_STACK_SIZE) randomStack = BIG_BLIND + (randomStack - DEFAULT_STACK_SIZE -1) % (DEFAULT_STACK_SIZE - BIG_BLIND);
                    }
                }
                usedStacks.insert(randomStack);
                players[i]->reset(isNewRound, randomStack); // isNewRound is false here
            }
        } else { // Not end_with_round==0 and not IS_EVALUATING
            // Original default stack logic
            for (auto p : players) {
                p->reset(isNewRound); // isNewRound is false here, so resets to startingStack
            }
        }
    } else { // isNewRound is true
        // Original logic for preserving stacks
        for (auto p : players) {
            p->reset(isNewRound); // isNewRound is true here, so preserves stack
        }
    }
    _calculateRewardScalar();

    // 位置设置逻辑（与第一个reset函数保持一致）
    if (fix_utg_position >= 0 && fix_utg_position < N_SEATS) {
        // 固定UTG位置时，需要根据UTG反推按钮位置
        if (N_SEATS == 2) {
            // 两人桌：UTG就是SB/BTN，另一个是BB
            buttonPos = fix_utg_position;
            sbPos = buttonPos;
            bbPos = (buttonPos + 1) % N_SEATS;
        } else if (N_SEATS > 2) {
            // 多人桌：UTG是BB+1，所以BB位置是UTG-1，按钮位置是BB-2
            bbPos = (fix_utg_position - 1 + N_SEATS) % N_SEATS;
            sbPos = (bbPos - 1 + N_SEATS) % N_SEATS;
            buttonPos = (sbPos - 1 + N_SEATS) % N_SEATS;
        } else {
            // N_SEATS <= 1的特殊情况
            buttonPos = 0; sbPos = 0; bbPos = 0;
        }
    } else {
        // 使用默认位置逻辑
        if (isNewRound) {
            buttonPos = (buttonPos + 1) % N_SEATS;
        } else {
            std::uniform_int_distribution<int> buttonDist(0, N_SEATS - 1);
            buttonPos = buttonDist(m_rng);
        }

        if (N_SEATS == 2) {
            sbPos = buttonPos;
            bbPos = (buttonPos + 1) % N_SEATS;
        } else if (N_SEATS > 2) {
            sbPos = (buttonPos + 1) % N_SEATS;
            bbPos = (buttonPos + 2) % N_SEATS;
        } else {
            sbPos = 0; bbPos = 0;
        }
    }

    mainPot = 0;
    sidePots.clear();
    currentMainPot = 0;
    currentSidePots.clear();
    currentSidePots.resize(N_SEATS, 0);
    lastRaiser = -1;
    nRaisesThisRound = 0;
    lastAction_member = {-1, -1, -1};
    nActionsThisEpisode = 0;
    cappedRaise_member.reset();
    handIsOver = false;

    // 根据公共牌数量设置当前轮次
    int num_board_cards = board_cards.size();
    if (num_board_cards == 0) {
        currentRound = PREFLOP;
    } else if (num_board_cards == 3) {
        currentRound = FLOP;
    } else if (num_board_cards == 4) {
        currentRound = TURN;
    } else if (num_board_cards == 5) {
        currentRound = RIVER;
    } else {
        // 无效的公共牌数量，默认为PREFLOP
        currentRound = PREFLOP;
        printf("警告: 无效的公共牌数量 %d，默认设置为PREFLOP\n", num_board_cards);
    }

    _postAntes();
    _putCurrentBetsIntoMainPotAndSidePots();
    _postSmallBlind();
    _postBigBlind();

    // 记录哪些玩家已经设置了手牌
    std::vector<bool> player_hand_set(N_SEATS, false);

    // 设置公共牌
    if (!board_cards.empty()) {

        int max_board_cards = std::min(static_cast<int>(board_cards.size()), 5);
        for (int i = 0; i < max_board_cards; ++i) {
            int card_idx = board_cards[i];
            if (card_idx >= 0 && card_idx < 52) {
                // 创建Card对象 (索引 = suit * 13 + value)
                Card::Suit suit = static_cast<Card::Suit>(card_idx % 4);
                Card::CardValue value = static_cast<Card::CardValue>(card_idx / 4);

                // 从 Deck 中移除该卡牌
                if (!deck->removeCard(suit, value)) {
                    // 移除失败，说明卡牌不在 Deck 中（已被使用/重复）
                    std::string card_str = getCardString(value, suit);
                    throw std::runtime_error("Duplicate card detected in board cards: " + card_str + " is already specified");
                }

                communityCards[i] = new Card(suit, value);
            } else {
                printf("警告: 公共牌索引无效 (%d)，跳过该牌\n", card_idx);
            }
        }

    }

    // 设置指定玩家的手牌
    if (!hole_cards.empty()) {
        int max_players = std::min(static_cast<int>(hole_cards.size()), N_SEATS);
        for (int i = 0; i < max_players; ++i) {
            if (hole_cards[i].size() >= 2) {
                int card1_idx = hole_cards[i][0];
                int card2_idx = hole_cards[i][1];

                // 确保卡牌索引在有效范围内 (0-51)
                if (card1_idx >= 0 && card1_idx < 52 && card2_idx >= 0 && card2_idx < 52) {
                    // 检查同一玩家手牌是否重复
                    if (card1_idx == card2_idx) {
                        Card::Suit suit = static_cast<Card::Suit>(card1_idx % 4);
                        Card::CardValue value = static_cast<Card::CardValue>(card1_idx / 4);
                        std::string card_str = getCardString(value, suit);
                        throw std::runtime_error("Duplicate card detected for player " + std::to_string(i) + ": both hole cards are " + card_str);
                    }

                    // 创建Card对象 (索引 = suit * 13 + value)
                    Card::Suit suit1 = static_cast<Card::Suit>(card1_idx % 4);
                    Card::CardValue value1 = static_cast<Card::CardValue>(card1_idx / 4);
                    Card::Suit suit2 = static_cast<Card::Suit>(card2_idx % 4);
                    Card::CardValue value2 = static_cast<Card::CardValue>(card2_idx / 4);

                    // 从 Deck 中移除这两张卡牌
                    bool card1_removed = deck->removeCard(suit1, value1);
                    bool card2_removed = deck->removeCard(suit2, value2);

                    if (!card1_removed || !card2_removed) {
                        // 如果任何一张卡牌移除失败，说明有重复
                        std::string error_msg = "Duplicate card detected for player " + std::to_string(i) + ": ";
                        if (!card1_removed && !card2_removed) {
                            std::string card1_str = getCardString(value1, suit1);
                            std::string card2_str = getCardString(value2, suit2);
                            error_msg += "both " + card1_str + " and " + card2_str + " are already specified";
                        } else if (!card1_removed) {
                            std::string card1_str = getCardString(value1, suit1);
                            error_msg += card1_str + " is already specified";
                        } else if (!card2_removed) {
                            std::string card2_str = getCardString(value2, suit2);
                            error_msg += card2_str + " is already specified";
                        }
                        throw std::runtime_error(error_msg);
                    }

                    // 确保hand有足够的空间
                    if (players[i]->hand.size() < 2) {
                        players[i]->hand.resize(2);
                    }

                    // 创建新的Card对象并分配给玩家
                    players[i]->hand[0] = new Card(suit1, value1);
                    players[i]->hand[1] = new Card(suit2, value2);
                    player_hand_set[i] = true;
                } else {
                    printf("警告: 玩家 %d 的手牌索引无效 (%d, %d)，将从剩余牌中随机发牌\n", i, card1_idx, card2_idx);
                }
            }
        }
    }

        // 为没有设置手牌的玩家从剩余deck中发牌
    for (int i = 0; i < N_SEATS; ++i) {
        if (!player_hand_set[i]) {
            // 为玩家i从剩余deck中发两张牌
            if (deck->getRemainingCardCount() >= 2) {
                // 确保hand有足够的空间
                if (players[i]->hand.size() < 2) {
                    players[i]->hand.resize(2);
                }

                // 使用popCard()方法从deck中取两张牌
                Card* card1 = deck->popCard();
                Card* card2 = deck->popCard();

                if (card1 && card2) {
                    players[i]->hand[0] = new Card(*card1); // 复制card对象
                    players[i]->hand[1] = new Card(*card2); // 复制card对象
                } else {
                    printf("警告: deck中取牌失败，玩家 %d 无法获得手牌\n", i);
                }
            } else {
                printf("警告: deck中剩余卡牌不足，玩家 %d 无法获得手牌\n", i);
            }
        }
    }

    // 建立初始手牌强度缓存
    for (int i = 0; i < N_SEATS; ++i) {
        if (players[i] && players[i]->hand.size() == 2 && players[i]->hand[0] && players[i]->hand[1]) {
            int handRank = getHandValuebyPlayer(i); // 1-169排名，1=最强，169=最弱
            // 转换为0-1范围的强度值：强度 = (170 - rank) / 169
            // AA(rank=1) -> strength=169/169=1.0, 72o(rank=169) -> strength=1/169≈0.006
            _initialHandStrengthCache[i] = static_cast<float>(170 - handRank) / 169.0f;
        } else {
            _initialHandStrengthCache[i] = 0.0f; // 无效手牌
        }
    }

    // 设置当前玩家
    if (currentRound == PREFLOP) {
        currentPlayer = _getFirstToActPreFlop();
    } else {
        currentPlayer = _getFirstToActPostFlop();
    }


    _updateHandPotentialForAllPlayers();

    // 计算并存储当前观察值到历史中
    std::vector<float> currentObservation = _calculateCurrentObservationByConfig();
    observationHistory.push_back(currentObservation);

    return getObservationForTransformer();
}

void PokerEnv::reset(bool is_eval_sim,
                     const std::vector<std::string>& player_specific_hole_cards_str,
                     const std::vector<std::vector<int>>& player_specific_hole_cards_value,
                     const std::string& board_cards_str,
                     const std::vector<int>& board_cards_value,
                     const std::vector<int>& deck_value,
                     const std::vector<int>& starting_stacks_config,
                     int max_rounds_per_hand_param) {

#ifdef DEBUG_POKER_ENV
    std::cout << "reset: Complex reset function called with:" << std::endl;
    std::cout << "  board_cards_str: '" << board_cards_str << "'" << std::endl;
    std::cout << "  board_cards_value.size(): " << board_cards_value.size() << std::endl;
    std::cout << "  board_cards_value: [";
    for (size_t i = 0; i < board_cards_value.size(); ++i) {
        std::cout << board_cards_value[i];
        if (i < board_cards_value.size() - 1) std::cout << ", ";
    }
    std::cout << "]" << std::endl;
#endif

    std::fill(actions_this_street.begin(), actions_this_street.end(), 0); // 重置行动次数
    observationHistory.clear();
    // 重置历史动作记录
    actionHistory.clear();

    // 0. 设置最大轮数限制
    max_rounds_per_hand = max_rounds_per_hand_param;

    // 1. Player Stack Setup
    for (int i = 0; i < N_SEATS; ++i) {
        int stack_size = DEFAULT_STACK_SIZE;
        if (is_eval_sim) {
            if (i < static_cast<int>(starting_stacks_config.size())) {
                stack_size = starting_stacks_config[i];
            } else if (!startingStackSizesList.empty() && i < static_cast<int>(startingStackSizesList.size())) {
                stack_size = startingStackSizesList[i];
            } else {
                std::uniform_int_distribution<int> stackDist(BIG_BLIND, DEFAULT_STACK_SIZE);
                stack_size = stackDist(m_rng);
            }
        }
        // Clear player's hand of any PokerEnv-owned cards before PokerPlayer::reset potentially clears the vector
        for (Card* c : players[i]->hand) delete c;
        players[i]->hand.clear();
        players[i]->reset(false, stack_size); // isNewRound=false for full player state reset
    }
    _calculateRewardScalar();

    // 2. Deck Setup
    deck->spaceTheDeck(); // Clears Deck's internal storage, re-initializes all_cards_storage, and refills current_deck
    deck->shuffle();      // Shuffles current_deck

    // Cards are now directly removed from deck when specified, no need for tracking

    // 3. General State Reset (Pots, Button, Round Info)
    // 位置设置逻辑（与其他reset函数保持一致）
    if (fix_utg_position >= 0 && fix_utg_position < N_SEATS) {
        // 固定UTG位置时，需要根据UTG反推按钮位置
        if (N_SEATS == 2) {
            // 两人桌：UTG就是SB/BTN，另一个是BB
            buttonPos = fix_utg_position;
            sbPos = buttonPos;
            bbPos = (buttonPos + 1) % N_SEATS;
        } else if (N_SEATS > 2) {
            // 多人桌：UTG是BB+1，所以BB位置是UTG-1，按钮位置是BB-2
            bbPos = (fix_utg_position - 1 + N_SEATS) % N_SEATS;
            sbPos = (bbPos - 1 + N_SEATS) % N_SEATS;
            buttonPos = (sbPos - 1 + N_SEATS) % N_SEATS;
        } else {
            // N_SEATS <= 1的特殊情况
            buttonPos = 0; sbPos = 0; bbPos = 0;
        }
    } else {
        // 使用默认位置逻辑
        if (!is_eval_sim) {
            std::uniform_int_distribution<int> buttonDist(0, N_SEATS - 1);
            buttonPos = buttonDist(m_rng);
        }
        // ... (rest of button, blinds, pot state reset as before) ...
        if (N_SEATS == 2) {
            sbPos = buttonPos;
            bbPos = (buttonPos + 1) % N_SEATS;
        } else if (N_SEATS > 2) {
            sbPos = (buttonPos + 1) % N_SEATS;
            bbPos = (buttonPos + 2) % N_SEATS;
        } else {
            sbPos = 0; bbPos = 0;
        }
    }
    mainPot = 0;
    sidePots.clear();
    currentMainPot = 0;
    currentSidePots.clear();
    currentSidePots.resize(N_SEATS, 0);
    lastRaiser = -1;
    nRaisesThisRound = 0;
    lastAction_member = {-1, -1, -1};
    nActionsThisEpisode = 0;
    cappedRaise_member.reset();
    handIsOver = false;
    currentRound = PREFLOP;

    // Clear PokerEnv-owned community cards
    for (Card* c : communityCards) delete c;
    std::fill(communityCards.begin(), communityCards.end(), nullptr);

    // 4. Community Cards (Board) Setup
    bool board_set_from_string = false;
    if (!board_cards_str.empty()) {
        std::vector<Card> temp_board_card_objects;
        _parseBoardString(board_cards_str, temp_board_card_objects); // Parses into Card objects

        int community_idx = 0;
        for (const auto& card_obj : temp_board_card_objects) {
            if (community_idx < N_COMMUNITY_CARDS) {
                // Use Deck to check for duplicates: try to remove card from deck
                if (!deck->removeCard(card_obj.getSuit(), card_obj.getValue())) {
                    // Card removal failed, it's a duplicate
                    std::string card_str = getCardString(card_obj.getValue(), card_obj.getSuit());
                    throw std::runtime_error("Duplicate card detected in board cards (string): " + card_str + " is already specified");
                }
                communityCards[community_idx] = new Card(card_obj.getSuit(), card_obj.getValue()); // PokerEnv owns this
                community_idx++;
                board_set_from_string = true;
            } else break;
        }
    }

    if (!board_set_from_string && !board_cards_value.empty()) {
        int community_idx = 0;
        for (int card_val_int : board_cards_value) {
            if (community_idx < N_COMMUNITY_CARDS && card_val_int >= 0 && card_val_int < 52) {
                Card::Suit s = static_cast<Card::Suit>(card_val_int % 4);
                Card::CardValue v = static_cast<Card::CardValue>(card_val_int / 4);

                // Use Deck to check for duplicates: try to remove card from deck
                if (!deck->removeCard(s, v)) {
                    // Card removal failed, it's a duplicate
                    std::string card_str = getCardString(v, s);
                    throw std::runtime_error("Duplicate card detected in board cards: " + card_str + " is already specified");
                }

                communityCards[community_idx] = new Card(s, v); // PokerEnv owns this
                community_idx++;
            } else if (community_idx >= N_COMMUNITY_CARDS) break;
        }
    }

    // Update currentRound based on set community cards
    int num_actual_board_cards = 0;
    for(const Card* c : communityCards) if(c) num_actual_board_cards++;

    if (num_actual_board_cards == 0) currentRound = PREFLOP;
    else if (num_actual_board_cards == 3) currentRound = FLOP;
    else if (num_actual_board_cards == 4) currentRound = TURN;
    else if (num_actual_board_cards == 5) currentRound = RIVER;
    else { currentRound = PREFLOP; /* Optionally log warning */ }


    // 5. Hole Cards Setup
    std::vector<bool> player_card_set(N_SEATS, false);
    bool any_hole_cards_specified = !player_specific_hole_cards_str.empty() || !player_specific_hole_cards_value.empty();

    if (any_hole_cards_specified) {
        for (int i = 0; i < N_SEATS; ++i) {
            // players[i]->hand was cleared and PokerPlayer::reset called earlier.
            // It should be an empty vector now.
            players[i]->hand.resize(N_HOLE_CARDS, nullptr); // Prepare space

            bool p_card_set_this_iter = false;
            // Priority 1: String input
            if (i < static_cast<int>(player_specific_hole_cards_str.size()) && !player_specific_hole_cards_str[i].empty()) {
                const std::string& two_cards_s = player_specific_hole_cards_str[i];
                std::vector<Card> parsed_hole_cards_temp; // Temporary storage for parsed Card objects
                // Attempt to parse the two_cards_s string into two Card objects.
                // This requires a robust parser. For now, let's use parseCardStringInternal for two parts.
                std::string s1_str, s2_str;
                // Simplified parsing logic here, assume it splits two_cards_s into s1_str and s2_str
                // A more robust split is in getRangeIdxByHand(const std::string& twoCardsStr) or similar
                size_t len = two_cards_s.length();
                std::string stripped_s;
                for(char ch : two_cards_s) if (!isspace(ch)) stripped_s += ch;
                len = stripped_s.length();

                if (len == 4) { // AsKd
                    s1_str = stripped_s.substr(0, 2); s2_str = stripped_s.substr(2, 2);
                } else if (len == 5) { // 10sKd or As10d
                    if (isdigit(stripped_s[1])) { s1_str = stripped_s.substr(0, 3); s2_str = stripped_s.substr(3, 2); }
                    else { s1_str = stripped_s.substr(0, 2); s2_str = stripped_s.substr(2, 3); }
                } else if (len == 6 && isdigit(stripped_s[1]) && isdigit(stripped_s[4])) { // 10s10d
                     s1_str = stripped_s.substr(0, 3); s2_str = stripped_s.substr(3, 3);
                } else {
                    // Try space split as fallback
                    size_t space_pos = two_cards_s.find(' ');
                    if (space_pos != std::string::npos) {
                        s1_str = two_cards_s.substr(0, space_pos);
                        size_t s2_start = two_cards_s.find_first_not_of(' ', space_pos);
                        if (s2_start != std::string::npos) s2_str = two_cards_s.substr(s2_start);
                    }
                }


                if (!s1_str.empty() && !s2_str.empty()) {
                    try {
                        auto pcard1_val_suit = parseCardStringInternal(s1_str);
                        auto pcard2_val_suit = parseCardStringInternal(s2_str);

                        // Check for same card in player's hand first
                        if (pcard1_val_suit.first == pcard2_val_suit.first && pcard1_val_suit.second == pcard2_val_suit.second) {
                            std::string card_str = getCardString(pcard1_val_suit.first, pcard1_val_suit.second);
                            throw std::runtime_error("Duplicate card detected for player " + std::to_string(i) + " (string input): both hole cards are the same: " + card_str);
                        }

                        // Use Deck to check for duplicates: try to remove cards from deck
                        bool card1_removed = deck->removeCard(pcard1_val_suit.second, pcard1_val_suit.first);
                        bool card2_removed = deck->removeCard(pcard2_val_suit.second, pcard2_val_suit.first);

                        if (!card1_removed || !card2_removed) {
                            // If any card removal failed, it's a duplicate
                            std::string error_msg = "Duplicate card detected for player " + std::to_string(i) + " (string input): ";

                            if (!card1_removed && !card2_removed) {
                                std::string card1_str = getCardString(pcard1_val_suit.first, pcard1_val_suit.second);
                                std::string card2_str = getCardString(pcard2_val_suit.first, pcard2_val_suit.second);
                                error_msg += "both " + card1_str + " and " + card2_str + " are already specified";
                            } else if (!card1_removed) {
                                std::string card1_str = getCardString(pcard1_val_suit.first, pcard1_val_suit.second);
                                error_msg += card1_str + " is already specified";
                            } else if (!card2_removed) {
                                std::string card2_str = getCardString(pcard2_val_suit.first, pcard2_val_suit.second);
                                error_msg += card2_str + " is already specified";
                            }

                            throw std::runtime_error(error_msg);
                        } else {
                            players[i]->hand[0] = new Card(pcard1_val_suit.second, pcard1_val_suit.first); // PokerEnv owns
                            players[i]->hand[1] = new Card(pcard2_val_suit.second, pcard2_val_suit.first); // PokerEnv owns

                            player_card_set[i] = true;
                            p_card_set_this_iter = true;
                        }
                    } catch (const std::invalid_argument& e) { /* Parsing failed, log warning */ }
                }
            }

            // Priority 2: Integer input
            if (!p_card_set_this_iter && i < static_cast<int>(player_specific_hole_cards_value.size()) && player_specific_hole_cards_value[i].size() >= N_HOLE_CARDS) {
                const std::vector<int>& p_cards_val = player_specific_hole_cards_value[i];
                int c1_val_int = p_cards_val[0];
                int c2_val_int = p_cards_val[1];

                if (c1_val_int >= 0 && c1_val_int < 52 && c2_val_int >= 0 && c2_val_int < 52) {
                    // Check for same card in player's hand first
                    if (c1_val_int == c2_val_int) {
                        Card::Suit suit = static_cast<Card::Suit>(c1_val_int % 4);
                        Card::CardValue value = static_cast<Card::CardValue>(c1_val_int / 4);
                        std::string card_str = getCardString(value, suit);
                        throw std::runtime_error("Duplicate card detected for player " + std::to_string(i) + ": both hole cards are " + card_str);
                    }

                    Card::Suit s1 = static_cast<Card::Suit>(c1_val_int % 4); Card::CardValue v1 = static_cast<Card::CardValue>(c1_val_int / 4);
                    Card::Suit s2 = static_cast<Card::Suit>(c2_val_int % 4); Card::CardValue v2 = static_cast<Card::CardValue>(c2_val_int / 4);

                    // Use Deck to check for duplicates: try to remove cards from deck
                    bool card1_removed = deck->removeCard(s1, v1);
                    bool card2_removed = deck->removeCard(s2, v2);

                    if (!card1_removed || !card2_removed) {
                        // If any card removal failed, it's a duplicate
                        std::string error_msg = "Duplicate card detected for player " + std::to_string(i) + ": ";

                        if (!card1_removed && !card2_removed) {
                            std::string card1_str = getCardString(v1, s1);
                            std::string card2_str = getCardString(v2, s2);
                            error_msg += "both " + card1_str + " and " + card2_str + " are already specified";
                        } else if (!card1_removed) {
                            std::string card1_str = getCardString(v1, s1);
                            error_msg += card1_str + " is already specified";
                        } else if (!card2_removed) {
                            std::string card2_str = getCardString(v2, s2);
                            error_msg += card2_str + " is already specified";
                        }

                        throw std::runtime_error(error_msg);
                    } else {
                        players[i]->hand[0] = new Card(s1, v1); // PokerEnv owns
                        players[i]->hand[1] = new Card(s2, v2); // PokerEnv owns

                        player_card_set[i] = true;
                    }
                } else { /* Invalid integer cards, log warning */ }
            }
        }
    }

    // Cards are already removed from deck when specified above, no batch removal needed

    // 7. Deal hole cards to players whose cards were not specified
    for (int i = 0; i < N_SEATS; ++i) {
        if (!player_card_set[i]) {
            // players[i]->hand should already be empty or full of nullptrs from earlier
            // and resized to N_HOLE_CARDS.
            for (int j = 0; j < N_HOLE_CARDS; ++j) {
                Card* drawn_card_ptr = deck->drawCard(); // Card* is owned by Deck
                if (drawn_card_ptr) {
                    // PokerEnv makes its own copy to own for its player hands
                    players[i]->hand[j] = new Card(drawn_card_ptr->getSuit(), drawn_card_ptr->getValue());
                } else {
                    // Deck is empty - this is an error condition if we need to deal more.
                    players[i]->hand[j] = nullptr; // No card available
                    // std::cerr << "Error: Deck ran out of cards dealing to player " << i << std::endl;
                }
            }
        }
    }

    // Handle the case where no specific cards were set for anyone and no custom deck.
    // This part seems redundant if the above logic correctly fills hands.
    // The previous logic section from line 787-795 regarding `no_specific_hole_cards_at_all`
    // and calling `_dealHoleCards()` can be removed if the new loop (737-756) correctly
    // deals to unspecified players by copying from `deck->drawCard()`.
    // The old `_dealHoleCards()` assigns `deck->drawCard()` pointers directly, which we are avoiding
    // for PokerEnv's direct ownership of hand cards.

    // 8. Post Blinds/Antes
    _postAntes();
    _putCurrentBetsIntoMainPotAndSidePots();
    _postSmallBlind();
    _postBigBlind();

    // 9. Determine Current Player
    if (currentRound == PREFLOP) {
        currentPlayer = _getFirstToActPreFlop();
    } else {
        currentPlayer = _getFirstToActPostFlop();
    }
    lastRaiser = (currentRound == PREFLOP && N_SEATS > 0) ? bbPos : _getFirstToActPostFlop();

    // 建立初始手牌强度缓存
    for (int i = 0; i < N_SEATS; ++i) {
        if (players[i] && players[i]->hand.size() == 2 && players[i]->hand[0] && players[i]->hand[1]) {
            int handRank = getHandValuebyPlayer(i); // 1-169排名，1=最强，169=最弱
            // 转换为0-1范围的强度值：强度 = (170 - rank) / 169
            // AA(rank=1) -> strength=169/169=1.0, 72o(rank=169) -> strength=1/169≈0.006
            _initialHandStrengthCache[i] = static_cast<float>(170 - handRank) / 169.0f;
        } else {
            _initialHandStrengthCache[i] = 0.0f; // 无效手牌
        }
    }

    _updateHandPotentialForAllPlayers();
    // 10. Initial Observation
    std::vector<float> currentObservation = _calculateCurrentObservationByConfig(); // Ensure this uses final state
    observationHistory.push_back(currentObservation);

    // Function is void.
}

std::tuple<std::vector<std::vector<float>>, std::vector<float>, std::vector<float>, bool> PokerEnv::step(int actionInt) {
    // This is the discrete action step.
    // It converts the discrete actionInt into a specific action type and amount,
    // validates it, and then calls the (actionType, amount) version of step.

    // Get the environment-adjusted action formulation (potential type and amount)
    std::vector<float> adjustedAction = _getEnvAdjustedActionFormulation(actionInt);

    // Get the fixed (validated and possibly modified) action
    std::vector<float> fixedAction = _getFixedAction(adjustedAction);

    int finalActionType = static_cast<int>(fixedAction[0]);
    float finalAmount = fixedAction[1]; // For FOLD, amount is -1. For CHECK_CALL/BET_RAISE, it's the total bet amount.

    // Call the step function that takes actionType and amount, passing the original actionInt
    return step(finalActionType, finalAmount, actionInt);
}


std::tuple<std::vector<std::vector<float>>, std::vector<float>, std::vector<float>, bool> PokerEnv::step(int actionTypeFromCaller, float amountFromCaller, int originalActionInt) {
    if (currentPlayer < 0 || currentPlayer >= N_SEATS || !players[currentPlayer]) {
        throw std::runtime_error("PokerEnv::step(actionType, amount): Invalid current player index: " + std::to_string(currentPlayer));
    }

    // 检查行动次数限制
    // this->max_rounds_per_hand 是指一条街上允许的单个玩家最大行动次数。
    if (this->max_rounds_per_hand > 0 && actions_this_street[currentPlayer] >= this->max_rounds_per_hand) {
        if (actionTypeFromCaller == BET_RAISE) {
            // 玩家已达到行动次数上限，且意图是BET_RAISE
            // 尝试将动作强制改为CHECK_CALL，如果CHECK_CALL不可行，则强制为FOLD

            // 构造一个虚拟的CHECK_CALL动作意图
            std::vector<float> hypothetical_check_call_intent = {static_cast<float>(CHECK_CALL), -1.0f};
            // 获取如果玩家尝试CHECK_CALL，实际会发生的动作
            std::vector<float> outcome_if_check_call = _getFixedAction(hypothetical_check_call_intent);

            if (outcome_if_check_call[0] == CHECK_CALL) {
                // CHECK_CALL 是可行的
                actionTypeFromCaller = CHECK_CALL;
                amountFromCaller = outcome_if_check_call[1]; // 使用_getFixedAction确定的金额
                #ifdef DEBUG_POKER_ENV
                std::cout << "[DEBUG] Player " << currentPlayer
                          << " action limit reached (" << actions_this_street[currentPlayer] << "/" << this->max_rounds_per_hand
                          << "). BET_RAISE forced to CHECK_CALL with amount " << amountFromCaller << "." << std::endl;
                #endif
            } else {
                // CHECK_CALL 不可行 (例如，_getFixedAction 将其转为 FOLD)
                actionTypeFromCaller = FOLD;
                amountFromCaller = -1.0f;
                #ifdef DEBUG_POKER_ENV
                std::cout << "[DEBUG] Player " << currentPlayer
                          << " action limit reached (" << actions_this_street[currentPlayer] << "/" << this->max_rounds_per_hand
                          << "). BET_RAISE forced to FOLD because CHECK_CALL was not viable (became type "
                          << outcome_if_check_call[0] << ")." << std::endl;
                #endif
            }
        }
        // 如果原意图是 FOLD 或 CHECK_CALL，或者已被强制修改为 FOLD/CHECK_CALL，则允许执行
    }

    std::vector<int> stacksBefore;
    for (const auto* p : players) stacksBefore.push_back(p->stack);

    PokerPlayer* player = players[currentPlayer];

    // Construct input for _getFixedAction with potentially modified actionTypeFromCaller and amountFromCaller
    std::vector<float> intendedActionVec = {static_cast<float>(actionTypeFromCaller), amountFromCaller};
    // Get the fixed (validated and possibly modified) action
    std::vector<float> fixedActionVec = _getFixedAction(intendedActionVec);

    int finalActionType = static_cast<int>(fixedActionVec[0]);
    float finalAmount = fixedActionVec[1]; // This is the target total currentBet for CHECK_CALL/BET_RAISE, or -1 for FOLD.

    // 记录历史动作 - 修正betAmount计算逻辑
    int betAmount = 0;
    if (finalActionType == BET_RAISE) {
        // 对于加注，记录实际加注额（总下注额减去之前的下注）
        betAmount = static_cast<int>(finalAmount) - player->currentBet;
    } else if (finalActionType == CHECK_CALL) {
        // 对于跟注，记录跟注额
        betAmount = static_cast<int>(finalAmount) - player->currentBet;
    }

        // 使用getPotSize()方法获取当前底池大小，确保一致性
    int currentPot = getPotSize();

    // 记录动作时的玩家筹码量（包括当前下注）
    int playerStackAtAction = player->stack + player->currentBet;

    // 如果没有提供原始actionInt，则需要计算
    int actionIntToRecord = originalActionInt;
    if (actionIntToRecord == -1) {
        // 使用历史上下文计算actionInt
        actionIntToRecord = _mapActionToFixedIndex(finalActionType, betAmount, currentPot, playerStackAtAction);
    }

    actionHistory.emplace_back(currentPlayer, finalActionType, betAmount, currentRound, currentPot, playerStackAtAction, actionIntToRecord);

    // 调试输出已移除

    if (finalActionType == FOLD) {
        player->fold();
    } else if (finalActionType == CHECK_CALL) {
        player->betRaise(static_cast<int>(finalAmount));
    } else if (finalActionType == BET_RAISE) {
        player->betRaise(static_cast<int>(finalAmount));
        lastRaiser = currentPlayer;
        nRaisesThisRound++;
    }

    // 在玩家行动后，增加其在本条街的行动次数
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) { // Defensive check
        actions_this_street[currentPlayer]++;
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] Player " << currentPlayer << " action count for street " << currentRound << " incremented to " << actions_this_street[currentPlayer] << std::endl;
        #endif
    }

    lastAction_member = {finalActionType, static_cast<int>(finalAmount), currentPlayer};
    nActionsThisEpisode++;

    // 实时计算边池状态
    _calculateCurrentSidePots();
    #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] step: player:"<< currentPlayer << "  action:" << finalActionType << "  amount:" << finalAmount << " actionint:" << originalActionInt << std::endl;
    #endif
    bool currentIsDone = _isHandDone();

    bool movedToNextRound = false;
    if (!currentIsDone && _isBettingDone()) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] step: calling _moveToNextRound" << std::endl;
        #endif
        _moveToNextRound();
        movedToNextRound = true;
        currentIsDone = _isHandDone(); // Check again after moving to next round
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] step: after _moveToNextRound, currentIsDone=" << currentIsDone << std::endl;
        #endif
    }

    if (currentIsDone) {
        _putCurrentBetsIntoMainPotAndSidePots(); // Collect final bets
        _assignRewardsAndResetBets();
        handIsOver = true;
    } else if (!movedToNextRound) {
        currentPlayer = findNextPlayerToAct(currentPlayer);
        if (currentPlayer == -1) {
            currentIsDone = true;
            _putCurrentBetsIntoMainPotAndSidePots();
            _assignRewardsAndResetBets();
            handIsOver = true;
        }
    }

    std::vector<float> rewards(N_SEATS, 0.0f);
    if (currentIsDone) {
        float totalReward = 0.0f;
        for (int i = 0; i < N_SEATS; ++i) {
            int stackDiff = players[i]->stack - stacksBefore[i];
            rewards[i] = static_cast<float>(stackDiff) / REWARD_SCALAR;
            totalReward += rewards[i];
        }
        if (N_SEATS > 0 && std::abs(totalReward) > 1e-5) {
            float correction = -totalReward / static_cast<float>(N_SEATS);
            for (int i = 0; i < N_SEATS; ++i) {
                rewards[i] += correction;
            }
        }
    }

    std::vector<float> currentObservation = _calculateCurrentObservationByConfig();
    observationHistory.push_back(currentObservation);

    // 使用getObservationForTransformer()并返回其结果加上rewards和currentIsDone
    auto transformer_result = getObservationForTransformer();
    auto sequence_features = std::get<0>(transformer_result);
    auto state_features = std::get<1>(transformer_result);

    return std::make_tuple(sequence_features, state_features, rewards, currentIsDone);
}

std::vector<std::vector<float>> PokerEnv::getPublicObservation() {
    // RNN优化：返回适合时序建模的观察序列
    const int MAX_SEQUENCE_LENGTH = 25;  // 限制序列长度，避免内存爆炸

    std::vector<std::vector<float>> sequence_observations;

    // 直接从历史记录中获取最近的观察，最多不超过MAX_SEQUENCE_LENGTH
    int start_idx = std::max(0, static_cast<int>(observationHistory.size()) - MAX_SEQUENCE_LENGTH);

    // 如果历史记录为空，则计算并添加当前观察
    if (observationHistory.empty()) {
        sequence_observations.push_back(_calculateCurrentObservationByConfig());
    } else {
        // 否则，从历史记录中提取序列
        for (size_t i = start_idx; i < observationHistory.size(); ++i) {
            sequence_observations.push_back(observationHistory[i]);
        }
    }


#ifdef DEBUG_POKER_ENV
    std::cout << "RNN sequence length: " << sequence_observations.size()
              << ", total history: " << observationHistory.size() << std::endl;
#endif

    return sequence_observations;
}

std::vector<float> PokerEnv::_calculateCurrentObservation() {
    // This needs to be ported from PokerEnv::getPublicObservation in PokerEnv.cpp
    // The PyTorch version constructs a large feature vector.
    // We need to replicate that feature construction logic.
    // For brevity, this will be a simplified version or placeholder.
    // A full porting of the observation vector is complex.

    const int NUM_RANKS = 13;
    const int NUM_SUITS = 4;
    const int MAX_COMMUNITY_CARDS = 5;
    const int TOTAL_ROUNDS = 4;
    const int MAX_PLAYERS_OBS = N_SEATS; // Use actual N_SEATS for obs

    // --- Get Canonical Suit Mapping for Isomorphism ---
    const auto& community_cards_for_iso = getCommunityCards();
    std::vector<int> canonical_suit_map = _getCanonicalSuitMap_static(community_cards_for_iso);

    // 修改归一化策略：使用大盲注作为基准，这样数值更稳定
    float normalizationSum = static_cast<float>(BIG_BLIND);
    if (normalizationSum <= 0.0f) normalizationSum = 1.0f;

    std::vector<float> allFeatures;
    // Reserve space (rough estimate)
    allFeatures.reserve(7 + 3 + MAX_PLAYERS_OBS + MAX_PLAYERS_OBS + TOTAL_ROUNDS +
                        MAX_PLAYERS_OBS + // side pots
                        MAX_PLAYERS_OBS * (2 + 2 + MAX_PLAYERS_OBS) + // player features
                        MAX_COMMUNITY_CARDS * (NUM_RANKS + NUM_SUITS));


    // Table State
    allFeatures.push_back(static_cast<float>(ANTE) / normalizationSum);
    allFeatures.push_back(static_cast<float>(SMALL_BLIND) / normalizationSum);
    allFeatures.push_back(static_cast<float>(BIG_BLIND) / normalizationSum);
    allFeatures.push_back(static_cast<float>(_getCurrentTotalMinRaise()) / normalizationSum);
    allFeatures.push_back(static_cast<float>(getPotSize()) / normalizationSum);
    allFeatures.push_back(static_cast<float>(getCurrentBet()) / normalizationSum); // total to call
    allFeatures.push_back((lastAction_member[0] == BET_RAISE) ? static_cast<float>(lastAction_member[1]) / normalizationSum : 0.0f);

    // Last Action Type (one-hot)
    std::vector<float> lastActionTypeFeat(3, 0.0f);
    if (lastAction_member[0] >= 0 && lastAction_member[0] < 3) lastActionTypeFeat[lastAction_member[0]] = 1.0f;
    else lastActionTypeFeat[CHECK_CALL] = 1.0f; // Default
    allFeatures.insert(allFeatures.end(), lastActionTypeFeat.begin(), lastActionTypeFeat.end());

    // Last Action Player (relative to button, one-hot)
    std::vector<float> lastActionPlayerRelPosFeat(MAX_PLAYERS_OBS, 0.0f);
    if (lastAction_member[2] >= 0 && lastAction_member[2] < N_SEATS) { // Check against N_SEATS for valid player
        int lastActorAbsPos = lastAction_member[2];
        int lastActorRelPos = (lastActorAbsPos - buttonPos + N_SEATS) % N_SEATS;
        if (lastActorRelPos >= 0 && lastActorRelPos < MAX_PLAYERS_OBS) { // Ensure relative pos is within observation bounds
            lastActionPlayerRelPosFeat[lastActorRelPos] = 1.0f;
        }
    }
    allFeatures.insert(allFeatures.end(), lastActionPlayerRelPosFeat.begin(), lastActionPlayerRelPosFeat.end());

    // Current Player (relative to button, one-hot)
    std::vector<float> currentPlayerRelPosFeat(MAX_PLAYERS_OBS, 0.0f);
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) { // Check against N_SEATS for valid player
        int currentPlayerAbsPos = currentPlayer;
        int currentPlayerRelPos = (currentPlayerAbsPos - buttonPos + N_SEATS) % N_SEATS;
        if (currentPlayerRelPos >= 0 && currentPlayerRelPos < MAX_PLAYERS_OBS) { // Ensure relative pos is within observation bounds
            currentPlayerRelPosFeat[currentPlayerRelPos] = 1.0f;
        }
    }
    allFeatures.insert(allFeatures.end(), currentPlayerRelPosFeat.begin(), currentPlayerRelPosFeat.end());

    // Current Round (one-hot)
    std::vector<float> currentRoundFeat(TOTAL_ROUNDS, 0.0f);
    if (currentRound >= 0 && currentRound < TOTAL_ROUNDS) currentRoundFeat[currentRound] = 1.0f;
    allFeatures.insert(allFeatures.end(), currentRoundFeat.begin(), currentRoundFeat.end());

    // Button Position (one-hot)
    std::vector<float> buttonPosFeat(MAX_PLAYERS_OBS, 0.0f);
    if (buttonPos >= 0 && buttonPos < MAX_PLAYERS_OBS) buttonPosFeat[buttonPos] = 1.0f;
    allFeatures.insert(allFeatures.end(), buttonPosFeat.begin(), buttonPosFeat.end());

    // Number of active players remaining (not folded, not all-in)
    int activePlayersRemaining = 0;
    for(const auto* p : players) {
        if (p && !p->folded && !p->isAllin && p->stack > 0) {
            activePlayersRemaining++;
        }
    }
    allFeatures.push_back(static_cast<float>(activePlayersRemaining) / N_SEATS); // Normalize by total seats

    // Number of raises this round
    allFeatures.push_back(static_cast<float>(nRaisesThisRound) / (N_SEATS > 0 ? N_SEATS : 1.0f)); // Normalize by N_SEATS or a typical max (e.g., 4)

    // Side Pots
    if (N_SEATS > 2) { // Only include for multiplayer for consistency with original
        for (int i = 0; i < MAX_PLAYERS_OBS; ++i) {
            allFeatures.push_back(i < currentSidePots.size() ? static_cast<float>(currentSidePots[i]) / normalizationSum : 0.0f);
        }
    }

    // Player Features
    for (int i = 0; i < MAX_PLAYERS_OBS; ++i) {
        if (i < N_SEATS) {
            const PokerPlayer* p = players[i];
            // 对于筹码相关特征，使用起始筹码作为归一化基准
            float stackNormFactor = static_cast<float>(DEFAULT_STACK_SIZE);
            if (stackNormFactor <= 0.0f) stackNormFactor = 1000.0f; // 默认值

            allFeatures.push_back(static_cast<float>(p->stack) / stackNormFactor);
            allFeatures.push_back(static_cast<float>(p->currentBet) / normalizationSum);
            allFeatures.push_back(p->hasActed ? 1.0f : 0.0f); // 新增：玩家是否已行动
            allFeatures.push_back(static_cast<float>(p->totalInvestedThisHand) / stackNormFactor); // 新增：玩家本手牌总投入

            // 新增：玩家相对于按钮的位置 (0: button, 1: button+1, ..., N_SEATS-1: button-1)
            float relativePosition = 0.0f;
            if (N_SEATS > 0) {
                relativePosition = static_cast<float>((p->seatId - buttonPos + N_SEATS) % N_SEATS);
            }
            allFeatures.push_back(N_SEATS > 1 ? relativePosition / (N_SEATS -1) : 0.0f); // 归一化

            // 新增：玩家本轮投入额
            allFeatures.push_back(static_cast<float>(p->investedThisRound) / normalizationSum); // 假设 p->investedThisRound 存在

            if (N_SEATS == 2) {
                allFeatures.push_back(p->isAllin ? 1.0f : 0.0f);
            } else {
                allFeatures.push_back(p->folded ? 1.0f : 0.0f);
                allFeatures.push_back(p->isAllin ? 1.0f : 0.0f);
                // Side Pot Rank (one-hot)
                for (int j = 0; j < MAX_PLAYERS_OBS; ++j) {
                    allFeatures.push_back((p->currentSidePotRank == j) ? 1.0f : 0.0f);
                }
            }
        } else { // Padding for non-existent players if MAX_PLAYERS_OBS > N_SEATS
            allFeatures.push_back(0.0f); // stack
            allFeatures.push_back(0.0f); // currentBet
            allFeatures.push_back(0.0f); // hasActed
            allFeatures.push_back(0.0f); // totalInvestedThisHand
            allFeatures.push_back(0.0f); // relativePosition
            allFeatures.push_back(0.0f); // investedThisRound
            if (N_SEATS == 2) {
                allFeatures.push_back(0.0f); // isAllin
            } else {
                allFeatures.push_back(0.0f); // folded
                allFeatures.push_back(0.0f); // isAllin
                for (int j = 0; j < MAX_PLAYERS_OBS; ++j) allFeatures.push_back(0.0f); // sidePotRank
            }
        }
    }

    // Community Cards (with suit isomorphism)
    const auto& ccards = community_cards_for_iso; // Reuse cards
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) {
        std::vector<float> rankFeats(NUM_RANKS, 0.0f);
        std::vector<float> suitFeats(NUM_SUITS, 0.0f);
        if (i < ccards.size() && ccards[i]) {
            int val = static_cast<int>(ccards[i]->getValue());
            int original_suit = static_cast<int>(ccards[i]->getSuit());
            if (val >= 0 && val < NUM_RANKS) rankFeats[val] = 1.0f;
            if (original_suit >= 0 && original_suit < NUM_SUITS) {
                int canonical_suit = canonical_suit_map[original_suit]; // Use canonical suit
                suitFeats[canonical_suit] = 1.0f;
            }
        }
        allFeatures.insert(allFeatures.end(), rankFeats.begin(), rankFeats.end());
        allFeatures.insert(allFeatures.end(), suitFeats.begin(), suitFeats.end());
    }

    // 新增：每个合法加注选项对应的具体总下注额 (归一化)
    const int MAX_RAISE_OPTIONS_IN_OBS = N_ACTIONS - 2; // 可配置的槽位数
    std::vector<float> raiseOptionAmounts(MAX_RAISE_OPTIONS_IN_OBS, -1.0f); // 用-1.0f作为未填充标记

    if (currentPlayer >= 0 && currentPlayer < N_SEATS && players[currentPlayer] && !players[currentPlayer]->folded && !players[currentPlayer]->isAllin) {
        std::vector<int> legalActs = getLegalActions();
        int currentRaiseOptionSlot = 0;
        for (int actionInt : legalActs) {
            if (actionInt >= BET_RAISE) { // 是一个加注动作
                if (currentRaiseOptionSlot < MAX_RAISE_OPTIONS_IN_OBS) {
                    // 获取此加注动作对应的总下注额
                    std::vector<float> adjustedAction = _getEnvAdjustedActionFormulation(actionInt);
                    std::vector<float> fixedAction = _getFixedAction(adjustedAction);
                    float totalBetAmount = fixedAction[1]; // _getFixedAction 返回的是总下注额
                    if (fixedAction[0] == BET_RAISE) { // 确保这确实是一个有效的加注
                         float stackNormFactor = static_cast<float>(DEFAULT_STACK_SIZE);
                         if (stackNormFactor <= 0.0f) stackNormFactor = 1000.0f;
                         raiseOptionAmounts[currentRaiseOptionSlot] = totalBetAmount / stackNormFactor;
                    }
                    currentRaiseOptionSlot++;
                }
            }
        }
    }
    allFeatures.insert(allFeatures.end(), raiseOptionAmounts.begin(), raiseOptionAmounts.end());

    // === 新增：扑克专用高级特征 ===

    // 1. 底池赔率 (Pot Odds)
    int totalToCall = getCurrentBet() - (currentPlayer >= 0 && currentPlayer < N_SEATS ? players[currentPlayer]->currentBet : 0);
    float potOdds = (getPotSize() > 0 && totalToCall > 0) ?
                    static_cast<float>(totalToCall) / static_cast<float>(getPotSize() + totalToCall) : 0.0f;
    allFeatures.push_back(potOdds);

    // 2. 有效筹码深度 (Effective Stack Depth)
    float effectiveStack = 0.0f;
    if (currentPlayer >= 0 && currentPlayer < N_SEATS && !players[currentPlayer]->folded) {
        int minStack = players[currentPlayer]->stack;
        for (const auto* p : players) {
            if (p && !p->folded && p->seatId != currentPlayer) {
                minStack = std::min(minStack, p->stack);
            }
        }
        float stackNormFactor = static_cast<float>(DEFAULT_STACK_SIZE);
        if (stackNormFactor <= 0.0f) stackNormFactor = 1000.0f;
        effectiveStack = static_cast<float>(minStack) / stackNormFactor;
    }
    allFeatures.push_back(effectiveStack);

        // 3. 位置强度指标 (Position Strength)
    float positionStrength = 0.0f;
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        int relativePos = (currentPlayer - buttonPos + N_SEATS) % N_SEATS;

        // 修复位置强度：BB应该是最弱位置
        // 位置顺序（从强到弱）：BTN=0, CO=1, MP=2, UTG=3, SB=N-2, BB=N-1
        float strength = 0.0f;
        if (relativePos == 0) {
            // BTN (按钮位置) - 最强
            strength = 1.0f;
        } else if (relativePos == (N_SEATS - 1)) {
            // BB (大盲) - 最弱位置
            strength = 0.0f;
        } else if (relativePos == (N_SEATS - 2)) {
            // SB (小盲) - 第二弱
            strength = 0.1f;  // 给SB一个很小的值，比BB强一点点
        } else {
            // 其他位置：CO, MP, UTG等，按距离BTN的远近线性分布
            // relativePos: 1(CO)最强 -> (N_SEATS-3)(UTG)最弱
            int earlyPositionCount = N_SEATS - 3;  // 除了BTN、SB、BB的位置数
                         if (earlyPositionCount > 0) {
                 // 从CO(relativePos=1)到UTG(relativePos=N_SEATS-3)的线性分布
                 float earlyPosStrength = static_cast<float>(earlyPositionCount - (relativePos - 1)) / static_cast<float>(earlyPositionCount);
                 strength = 0.2f + 0.7f * earlyPosStrength;  // 范围 [0.2, 0.9]，让CO略弱于BTN
             } else {
                 strength = 0.5f; // 兜底值
             }
        }
        positionStrength = strength;
    }
    allFeatures.push_back(positionStrength);

    // 4. 激进度指标 (Aggression Factor)
    float aggressionFactor = 0.0f;
    int totalRaises = 0, totalCalls = 0;
    // 这里需要根据您的动作历史记录来计算
    // 简化版本：基于当前轮的加注次数
    if (nRaisesThisRound > 0) {
        aggressionFactor = static_cast<float>(nRaisesThisRound) / static_cast<float>(nRaisesThisRound + 1);
    }
    allFeatures.push_back(aggressionFactor);

    // 5. 投入比例 (Investment Ratio)
    float investmentRatio = 0.0f;
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        float startingStackForPlayer = static_cast<float>(players[currentPlayer]->startingStack);
        if (startingStackForPlayer <= 0.0f) startingStackForPlayer = static_cast<float>(DEFAULT_STACK_SIZE);
        if (startingStackForPlayer > 0.0f) {
            investmentRatio = static_cast<float>(players[currentPlayer]->totalInvestedThisHand) / startingStackForPlayer;
        }
    }
    allFeatures.push_back(investmentRatio);

    // 6. 手牌阶段进度 (Hand Progress)
    float handProgress = static_cast<float>(currentRound) / 3.0f; // 0=preflop, 1=river
    allFeatures.push_back(handProgress);

    // 7. 玩家活跃度分布 (Player Activity Distribution)
    int foldedCount = 0, allinCount = 0, activeCount = 0;
    for (const auto* p : players) {
        if (p) {
            if (p->folded) foldedCount++;
            else if (p->isAllin) allinCount++;
            else activeCount++;
        }
    }
    allFeatures.push_back(static_cast<float>(foldedCount) / N_SEATS);
    allFeatures.push_back(static_cast<float>(allinCount) / N_SEATS);
    allFeatures.push_back(static_cast<float>(activeCount) / N_SEATS);

    // 8. 边池复杂度 (Side Pot Complexity)
    float sidePotComplexity = 0.0f;
    if (!sidePots.empty()) {
        sidePotComplexity = static_cast<float>(sidePots.size()) / static_cast<float>(N_SEATS);
    }
    allFeatures.push_back(sidePotComplexity);

    return allFeatures;
}

std::vector<float> PokerEnv::_calculateCurrentObservationSimplified() {
    // Transformer版本：使用可变长度的动作历史
    // 新增：筹码量信息、统一归一化、未行动玩家数量
    // 优化：移除游戏阶段特征（只训练翻前），移除位置强度向量（让模型自己学习），优化下注倍数表示

    // 定义固定的动作空间 - 与N_ACTIONS保持一致
    const int FIXED_ACTION_TYPES = N_ACTIONS; // 2 + betSizesListAsFracOfPot.size()

    std::vector<float> obs;

    // 计算动作特征大小
    int actionFeatureSize = N_SEATS + FIXED_ACTION_TYPES + 1; // 玩家位置 + 固定动作向量 + 下注倍数
    int stackFeatureSize = N_SEATS; // 每个玩家的筹码量（相对于初始筹码）
    int effectiveStackFeatureSize = 1; // 有效筹码量与底池比例
    int playersToActFeatureSize = 1; // 当前玩家之后还有多少人未行动
    int historyLengthFeatureSize = 1; // 动作历史长度（归一化）

    // 计算总大小：其他特征 + 动作历史长度 + 动作历史数据
    int totalSize = N_SEATS + stackFeatureSize + effectiveStackFeatureSize + playersToActFeatureSize +
                   historyLengthFeatureSize + actionHistory.size() * actionFeatureSize;
    obs.reserve(totalSize);

    // 1. 当前玩家位置 (N_SEATS个位置，one-hot编码) - 让模型自己学习位置价值
    std::vector<float> currentPlayerPos(N_SEATS, 0.0f);
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        // 计算当前玩家相对于按钮的位置
        int relativePos = (currentPlayer - buttonPos + N_SEATS) % N_SEATS;
        if (relativePos >= 0 && relativePos < N_SEATS) {
            currentPlayerPos[relativePos] = 1.0f;
        }
    }
    obs.insert(obs.end(), currentPlayerPos.begin(), currentPlayerPos.end());

    // 2. 每个玩家的筹码量（相对于初始筹码量）
    std::vector<float> stackSizes(N_SEATS, 0.0f);
    for (int i = 0; i < N_SEATS; i++) {
        if (players[i] && DEFAULT_STACK_SIZE > 0) {
            // 计算当前总筹码量（包括已下注的筹码）
            int totalChips = players[i]->stack + players[i]->currentBet;
            stackSizes[i] = static_cast<float>(totalChips) / static_cast<float>(DEFAULT_STACK_SIZE);
        }
    }
    obs.insert(obs.end(), stackSizes.begin(), stackSizes.end());

    // 3. 有效筹码量与当前底池的比例
    float effectiveStackToPotRatio = 0.0f;
    if (currentPlayer >= 0 && currentPlayer < N_SEATS && players[currentPlayer]) {
        // 使用getPotSize()方法获取当前底池大小，确保一致性
        int currentPot = getPotSize();
        if (currentPot == 0) currentPot = BIG_BLIND; // 避免除零

        // 计算有效筹码量：当前玩家与所有未弃牌对手之间的最小筹码量
        int currentPlayerTotalChips = players[currentPlayer]->stack + players[currentPlayer]->currentBet;
        int effectiveStack = currentPlayerTotalChips;

        // 找到最小对手筹码量
        for (int i = 0; i < N_SEATS; i++) {
            if (i != currentPlayer && players[i] && !players[i]->folded) {
                int opponentTotalChips = players[i]->stack + players[i]->currentBet;
                effectiveStack = std::min(effectiveStack, opponentTotalChips);

            }
        }


        effectiveStackToPotRatio = static_cast<float>(effectiveStack) / static_cast<float>(currentPot);
    }
    obs.push_back(std::log1p(effectiveStackToPotRatio)); // log(1 + x));


    // 5. 动作历史长度（归一化到[0,1]范围，假设最大长度为100）
    float normalizedHistoryLength = static_cast<float>(actionHistory.size()) / 100.0f;
    obs.push_back(normalizedHistoryLength);

    // 6. 动作历史记录（可变长度，放在最后）
    for (size_t historyIdx = 0; historyIdx < actionHistory.size(); historyIdx++) {
        // 玩家位置向量 (N_SEATS)
        std::vector<float> playerPos(N_SEATS, 0.0f);

        // 固定大小的动作向量 (FIXED_ACTION_TYPES)
        std::vector<float> actionTaken(FIXED_ACTION_TYPES, 0.0f);

        // 下注倍数 (1)
        float betMultiplier = 0.0f;

        // 从历史动作记录中获取数据（按时间顺序，最早的在前）
        const ActionRecord& record = actionHistory[historyIdx];

        // 设置玩家位置（相对于按钮位置）
        if (record.playerId >= 0 && record.playerId < N_SEATS) {
            // 计算相对位置：button=0, sb=1, bb=2, utg=3, ...
            int relativePos = (record.playerId - buttonPos + N_SEATS) % N_SEATS;
            if (relativePos >= 0 && relativePos < N_SEATS) {
                playerPos[relativePos] = 1.0f;
            }
        }

        // 直接使用记录的actionInt，避免重新计算
        int actionIndex = record.actionInt;
        if (actionIndex >= 0 && actionIndex < FIXED_ACTION_TYPES) {
            actionTaken[actionIndex] = 1.0f;
        }

        // 优化的下注倍数计算（相对于底池大小，对加注使用对数变换）
        if (record.betAmount > 0) {
            int historicalPot = record.potAtActionTime > 0 ? record.potAtActionTime : BIG_BLIND;

            betMultiplier = static_cast<float>(record.betAmount) / static_cast<float>(historicalPot);

        } else {
            betMultiplier = 0.0f;
        }

        // 添加到观察向量
        obs.insert(obs.end(), playerPos.begin(), playerPos.end());
        obs.insert(obs.end(), actionTaken.begin(), actionTaken.end());
        obs.push_back(betMultiplier);
    }

    // 验证观察向量大小是否正确（调试用）
    #ifdef DEBUG_POKER_ENV
    if (obs.size() != static_cast<size_t>(totalSize)) {
        std::cerr << "Warning: Observation vector size mismatch. Expected: " << totalSize
                  << ", Actual: " << obs.size() << std::endl;
    }
    #endif

    return obs;
}



// 实现getPublicObservationSimplified方法
std::vector<std::vector<float>> PokerEnv::getPublicObservationSimplified() {
    // 为每个玩家返回简化的公共观察
    std::vector<std::vector<float>> observations;
    observations.reserve(N_SEATS);

    for (int i = 0; i < N_SEATS; i++) {
        observations.push_back(_calculateCurrentObservationSimplified());
    }

    return observations;
}

// 辅助函数：将动作类型和下注金额映射到固定的动作索引
int PokerEnv::_mapActionToFixedIndex(int actionType, int betAmount) const {
    // 动作空间映射：
    // 0: FOLD
    // 1: CHECK_CALL
    // 2+: BET_RAISE (对应betSizesListAsFracOfPot中的不同加注大小)

    if (actionType == FOLD) {
        return 0;
    } else if (actionType == CHECK_CALL) {
        return 1;
    } else if (actionType == BET_RAISE) {
        // 获取当前底池大小
        int currentPot = mainPot;
        for (int sidePot : sidePots) {
            currentPot += sidePot;
        }

        // 如果底池为0，使用大盲注作为基准
        if (currentPot == 0) {
            currentPot = BIG_BLIND;
        }

        float potMultiplier = static_cast<float>(betAmount) / static_cast<float>(currentPot);

        // 检查是否是全押
        if (currentPlayer >= 0 && currentPlayer < N_SEATS && players[currentPlayer]) {
            int playerTotalChips = players[currentPlayer]->stack + players[currentPlayer]->currentBet;
            if (betAmount >= playerTotalChips) {
                // 全押映射到最后一个加注索引
                return N_ACTIONS - 1;
            }
        }

        // 根据底池倍数找到最接近的betSizesListAsFracOfPot索引
        float bestMatch = std::numeric_limits<float>::max();
        int bestIndex = 2; // 默认第一个加注选项

        for (size_t i = 0; i < betSizesListAsFracOfPot.size(); i++) {
            float targetMultiplier = betSizesListAsFracOfPot[i];
            float diff = std::abs(potMultiplier - targetMultiplier);
            if (diff < bestMatch) {
                bestMatch = diff;
                bestIndex = 2 + static_cast<int>(i);
            }
        }

        return bestIndex;
    }

    // 未知动作类型，返回-1表示无效
    return -1;
}

// 重载版本：支持历史上下文参数
int PokerEnv::_mapActionToFixedIndex(int actionType, int betAmount, int potAtActionTime, int playerStackAtActionTime) const {
    // 动作空间映射：
    // 0: FOLD
    // 1: CHECK_CALL
    // 2+: BET_RAISE (对应betSizesListAsFracOfPot中的不同加注大小)

    if (actionType == FOLD) {
        return 0;
    } else if (actionType == CHECK_CALL) {
        return 1;
    } else if (actionType == BET_RAISE) {
        // 使用历史上下文中的底池大小
        int historicalPot = potAtActionTime;

        // 如果底池为0，使用大盲注作为基准
        if (historicalPot == 0) {
            historicalPot = BIG_BLIND;
        }

        float potMultiplier = static_cast<float>(betAmount) / static_cast<float>(historicalPot);

        // 使用历史上下文中的玩家筹码量检查是否是全押
        if (betAmount >= playerStackAtActionTime) {
            // 全押映射到最后一个加注索引
            return N_ACTIONS - 1;
        }

        // 根据底池倍数找到最接近的betSizesListAsFracOfPot索引
        float bestMatch = std::numeric_limits<float>::max();
        int bestIndex = 2; // 默认第一个加注选项

        for (size_t i = 0; i < betSizesListAsFracOfPot.size(); i++) {
            float targetMultiplier = betSizesListAsFracOfPot[i];
            float diff = std::abs(potMultiplier - targetMultiplier);
            if (diff < bestMatch) {
                bestMatch = diff;
                bestIndex = 2 + static_cast<int>(i);
            }
        }

        return bestIndex;
    }

    // 未知动作类型，返回-1表示无效
    return -1;
}

// 辅助方法：根据配置标志选择观察计算方法
std::vector<float> PokerEnv::_calculateCurrentObservationByConfig() {
    if (use_simplified_observation) {
        return _calculateCurrentObservationSimplified();
    } else {
        return _calculateCurrentObservation();
    }
}

int64_t PokerEnv::getRangeIdx(int playerId) {

    if (playerId < 0 || playerId >= N_SEATS) {
        return -1;
    }
    const PokerPlayer* p = players[playerId];
    if (p->hand.size() < 2) { // Hands must have 2 cards
        return -1;
    }


    // --- 翻前 Pre-flop: 映射到169种手牌类型 ---
    // 计算实际非空的公共牌数量
    int actual_community_cards_count = 0;
    for (const Card* c : getCommunityCards()) {
        if (c != nullptr) actual_community_cards_count++;
    }

    // 翻前逻辑：实际公共牌为0或者end_with_round=0且currentRound=PREFLOP时，使用getHandValuebyPlayer
    bool should_use_preflop_encoding = (actual_community_cards_count == 0) ||
                                       (end_with_round == 0 && currentRound == PREFLOP);

    if (should_use_preflop_encoding) {

        // 使用 getHandValuebyPlayer 返回 1-169 排名，1=最强，169=最弱
        // 转换为 0-168 的索引：index = handRank - 1
        int handRank = getHandValuebyPlayer(playerId);
        return handRank - 1; // 转换为 0-168 范围
    }
    // --- Apply Suit Isomorphism to get Canonical Hand ---
    std::vector<int> canonical_suit_map = _getCanonicalSuitMap_static(getCommunityCards());

    int rank1 = p->hand[0]->getValue();
    int suit1 = canonical_suit_map[p->hand[0]->getSuit()];
    int canon_card1_idx = rank1 * 4 + suit1;

    int rank2 = p->hand[1]->getValue();
    int suit2 = canonical_suit_map[p->hand[1]->getSuit()];
    int canon_card2_idx = rank2 * 4 + suit2;

    // To create a unique index for the pair, always sort them (e.g., higher index first)
    // The standard formula for combinations C(n, k) is used, where n=52, k=2.
    // C(c1, 2) + c2
    int c1 = std::max(canon_card1_idx, canon_card2_idx);
    int c2 = std::min(canon_card1_idx, canon_card2_idx);

    // This formula generates a unique index from 0 to C(52, 2) - 1 = 1325.
    int range_idx = c1 * (c1 - 1) / 2 + c2;

    return range_idx;
}

// New function: getRangeIdxByHand(const Card* card1, const Card* card2)
int64_t PokerEnv::getRangeIdxByHand(const Card* card1, const Card* card2) {
    if (!card1 || !card2) {
        return -1; // Invalid cards
    }

    // 获取牌面值和花色
    int card1_value = static_cast<int>(card1->getValue()); // 0-12
    int card1_suit = static_cast<int>(card1->getSuit());   // 0-3
    int card2_value = static_cast<int>(card2->getValue()); // 0-12
    int card2_suit = static_cast<int>(card2->getSuit());   // 0-3

    // 转换为 1D 索引
    int card1_1d = card1_value * N_SUITS + card1_suit;
    int card2_1d = card2_value * N_SUITS + card2_suit;

    // 确保 c1 < c2
    if (card1_1d > card2_1d) {
        std::swap(card1_1d, card2_1d);
    }

    // 使用查找表
    auto it = m_rangeIdxLut.find(std::make_pair(card1_1d, card2_1d));
    if (it != m_rangeIdxLut.end()) {
        return it->second;
    }

    // 如果没找到，返回 -1 表示错误 (理论上不应该发生)
    #ifdef DEBUG_POKER_ENV
    std::cerr << "[ERROR getRangeIdxByHand(Card*, Card*)] Did not find (" << card1_1d << "," << card2_1d << ") in LUT!" << std::endl;
    #endif
    return -1;
}

// Overload: getRangeIdxByHand(const std::string& cardStr1, const std::string& cardStr2)
int64_t PokerEnv::getRangeIdxByHand(const std::string& cardStr1, const std::string& cardStr2) {
    try {
        auto parsedCard1 = parseCardStringInternal(cardStr1);
        auto parsedCard2 = parseCardStringInternal(cardStr2);

        Card c1_obj(parsedCard1.second, parsedCard1.first);
        Card c2_obj(parsedCard2.second, parsedCard2.first);

        return getRangeIdxByHand(&c1_obj, &c2_obj);
    } catch (const std::invalid_argument& e) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[DEBUG getRangeIdxByHand two_str] Error parsing card strings: " << e.what() << std::endl;
        #endif
        return -1; // Indicate error
    }
}

// New overload: getRangeIdxByHand(const std::string& twoCardsStr)
int64_t PokerEnv::getRangeIdxByHand(const std::string& twoCardsStr) {
    if (twoCardsStr.empty()) {
        return -1;
    }

    std::string cardStr1, cardStr2;
    size_t space_pos = twoCardsStr.find(' ');

    if (space_pos != std::string::npos) {
        cardStr1 = twoCardsStr.substr(0, space_pos);
        size_t first_char_card2 = twoCardsStr.find_first_not_of(' ', space_pos);
        if (first_char_card2 != std::string::npos) {
            cardStr2 = twoCardsStr.substr(first_char_card2);
        } else {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[DEBUG getRangeIdxByHand single_str] Malformed input with space: " << twoCardsStr << std::endl;
            #endif
            return -1;
        }
    } else {
        // No space found, iterate through all possible split points
        bool found_split = false;
        // Iterate from a minimum card length (e.g., 2 like "As") to a maximum practical length
        // Maximum length for a card: "10" (2) + Unicode suit (e.g. ♠ is 3 bytes) = 5.
        // Minimum total length for two cards: 2+2 = 4 (e.g. "AsKc")
        // Maximum total length for two cards: 5+5 = 10 (e.g. "10♠10♥")
        if (twoCardsStr.length() >= 4 && twoCardsStr.length() <= 10) { // Basic sanity check on total length
            for (size_t len_card1 = 2; len_card1 <= 5 && len_card1 < twoCardsStr.length(); ++len_card1) {
                std::string potential_card1_str = twoCardsStr.substr(0, len_card1);
                std::string potential_card2_str = twoCardsStr.substr(len_card1);

                if (potential_card2_str.length() < 2 || potential_card2_str.length() > 5) { // Sanity check for card 2 length
                    continue;
                }

                try {
                    parseCardStringInternal(potential_card1_str); // Throws on error
                    parseCardStringInternal(potential_card2_str); // Throws on error
                    // If both parsed successfully, this is our split
                    cardStr1 = potential_card1_str;
                    cardStr2 = potential_card2_str;
                    found_split = true;
                    break;
                } catch (const std::invalid_argument& e) {
                    // This split was invalid, try next length for card1
                    #ifdef DEBUG_POKER_ENV
                    // std::cout << "[DEBUG getRangeIdxByHand single_str] Split attempt failed for "
                    //           << potential_card1_str << " / " << potential_card2_str << ": " << e.what() << std::endl;
                    #endif
                }
            }
        }

        if (!found_split) {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[DEBUG getRangeIdxByHand single_str] Could not determine valid split for no-space string: " << twoCardsStr << std::endl;
            #endif
            return -1; // Could not split
        }
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getRangeIdxByHand single_str] Split '" << twoCardsStr << "' into: card1='" << cardStr1 << "', card2='" << cardStr2 << "'" << std::endl;
    #endif

    if (cardStr1.empty() || cardStr2.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[DEBUG getRangeIdxByHand single_str] One of the card strings is empty after split attempt for: " << twoCardsStr << std::endl;
        #endif
        return -1;
    }
    return getRangeIdxByHand(cardStr1, cardStr2); // Call the two-string overload
}

std::vector<float> PokerEnv::getRangePrivObs(int playerId) {
    int64_t range_idx = getRangeIdx(playerId);

    if (range_idx == -1) {
        // Invalid player ID or hand, return an empty or zeroed vector
        // consistent with LUT vector size.
        const int N_RANKS_CONST = 13;
        const int N_SUITS_CONST = 4;
        const int N_HOLE_CARDS_CONST = 2;
        bool suits_matter = true;
        if (args_config.contains("game_settings") && args_config["game_settings"].is_object() &&
            args_config["game_settings"].contains("suits_matter") && args_config["game_settings"]["suits_matter"].is_boolean()) {
            suits_matter = args_config["game_settings"]["suits_matter"].get<bool>();
        }
        const int D_per_card = suits_matter ? (N_RANKS_CONST + N_SUITS_CONST) : N_RANKS_CONST;
        return std::vector<float>(N_HOLE_CARDS_CONST * D_per_card, 0.0f);
    }

    auto it = m_privObsLut.find(range_idx);
    if (it != m_privObsLut.end()) {
        return it->second;
    } else {
#ifdef DEBUG_POKER_ENV
        std::cerr << "Error: range_idx " << range_idx << " not found in m_privObsLut. This should not happen if LUT is built correctly." << std::endl;
#endif
        // Fallback, though theoretically unreachable if range_idx is valid and LUT is correct.
        const int N_RANKS_CONST = 13;
        const int N_SUITS_CONST = 4;
        const int N_HOLE_CARDS_CONST = 2;
        bool suits_matter = true;
        if (args_config.contains("game_settings") && args_config["game_settings"].is_object() &&
            args_config["game_settings"].contains("suits_matter") && args_config["game_settings"]["suits_matter"].is_boolean()) {
            suits_matter = args_config["game_settings"]["suits_matter"].get<bool>();
        }
        const int D_per_card = suits_matter ? (N_RANKS_CONST + N_SUITS_CONST) : N_RANKS_CONST;
        return std::vector<float>(N_HOLE_CARDS_CONST * D_per_card, 0.0f);
    }
}


std::vector<float> PokerEnv::getLegalActionMask() {
    // This needs to call the more detailed getLegalActions() from PokerEnv.cpp
    // and then convert that to a mask.
    // The getLegalActions() in PokerEnv.cpp itself depends on _getFixedAction logic.
    // For now, a simplified version:
    // 获取合法动作列表
    std::vector<int> legalActions = getLegalActions();

    // 创建全0的掩码（所有动作不合法）
    std::vector<float> mask(N_ACTIONS, 0.0f);

    // 根据合法动作列表设置掩码
    for (int action : legalActions) {
        if (action >= 0 && action < N_ACTIONS) {
            mask[action] = 1.0f;
        }
    }

    return mask;
}

std::vector<int> PokerEnv::getLegalActions() {
    std::vector<int> legalActions;

    // 默认总是可以弃牌
    legalActions.push_back(FOLD);

    // 检查是否可以跟注/让牌
    std::vector<float> checkCallAction = {static_cast<float>(CHECK_CALL), -1}; // Use static_cast for clarity
    std::vector<float> fixedCheckCallAction = _getFixedAction(checkCallAction);

    if (fixedCheckCallAction[0] == CHECK_CALL) {
        legalActions.push_back(CHECK_CALL);
    }

    // 检查各种加注选项
    int lastTooSmall = -1;
    for (int a = 2; a < N_ACTIONS; ++a) {
        std::vector<float> raiseAction = _getEnvAdjustedActionFormulation(a);
        std::vector<float> fixedRaiseAction = _getFixedAction(raiseAction);

        // 如果想加注但环境不允许，就停止添加更大的加注选项
        if (raiseAction[0] != fixedRaiseAction[0]) {
            break;
        }

        // 如果加注额被调整为较小值，记录下来
        if (raiseAction[1] < fixedRaiseAction[1] && a < N_ACTIONS) {
            lastTooSmall = a;
        } else {
            // 如果有之前太小的加注，先添加
            if (lastTooSmall != -1) {
                legalActions.push_back(lastTooSmall);
                lastTooSmall = -1;
            }

            // 添加当前合法的加注
            legalActions.push_back(a);
        }

        // 如果加注被调整为较大值，更大的加注也会被调整为相同值，所以停止添加
        if (raiseAction[1] > fixedRaiseAction[1]) {
            break;
        }
    }

    return legalActions;
}

// --- Private helper method implementations (Ported from PokerEnv.cpp) ---

void PokerEnv::_dealHoleCards() {
    for (auto p : players) {
        // Delete cards PokerEnv might have new'd for this hand previously
        for (Card* c : p->hand) {
            delete c;
        }
        p->hand.clear();
        for (int i = 0; i < N_HOLE_CARDS; ++i) {
            Card* drawn_card_ptr = deck->drawCard();
            if (drawn_card_ptr) {
                p->hand.push_back(new Card(drawn_card_ptr->getSuit(), drawn_card_ptr->getValue())); // PokerEnv owns the new copy
            } else {
                p->hand.push_back(nullptr); // Should ideally not happen if deck has enough cards
            }
        }
    }
}

void PokerEnv::_dealFlop() {
    // Delete existing cards at these community card slots if any (owned by PokerEnv)
    for (int i = 0; i < N_FLOP_CARDS; ++i) {
        delete communityCards[i];
        communityCards[i] = nullptr;
    }
    deck->burnCard(); // Burn one card before flop
    for (int i = 0; i < N_FLOP_CARDS; ++i) {
        Card* drawn_card_ptr = deck->drawCard();
        if (drawn_card_ptr) {
            communityCards[i] = new Card(drawn_card_ptr->getSuit(), drawn_card_ptr->getValue()); // PokerEnv owns copy
        } else {
            communityCards[i] = nullptr; // Error or deck empty
        }
    }
}

void PokerEnv::_dealTurn() {
    // Delete existing card at this community card slot if any (owned by PokerEnv)
    delete communityCards[N_FLOP_CARDS];
    communityCards[N_FLOP_CARDS] = nullptr;

    deck->burnCard(); // Burn one card before turn
    Card* drawn_card_ptr = deck->drawCard();
    if (drawn_card_ptr) {
        communityCards[N_FLOP_CARDS] = new Card(drawn_card_ptr->getSuit(), drawn_card_ptr->getValue()); // PokerEnv owns copy
    } else {
        communityCards[N_FLOP_CARDS] = nullptr; // Error or deck empty
    }
}

void PokerEnv::_dealRiver() {
    // Delete existing card at this community card slot if any (owned by PokerEnv)
    delete communityCards[N_FLOP_CARDS + N_TURN_CARDS];
    communityCards[N_FLOP_CARDS + N_TURN_CARDS] = nullptr;

    deck->burnCard(); // Burn one card before river
    Card* drawn_card_ptr = deck->drawCard();
    if (drawn_card_ptr) {
        communityCards[N_FLOP_CARDS + N_TURN_CARDS] = new Card(drawn_card_ptr->getSuit(), drawn_card_ptr->getValue()); // PokerEnv owns copy
    } else {
        communityCards[N_FLOP_CARDS + N_TURN_CARDS] = nullptr; // Error or deck empty
    }
}

void PokerEnv::_dealNextRound() {
    if (currentRound == FLOP) _dealFlop();
    else if (currentRound == TURN) _dealTurn();
    else if (currentRound == RIVER) _dealRiver();

    // 发完公共牌后更新所有玩家的手牌强度和潜力
    _updateHandPotentialForAllPlayers();
}

void PokerEnv::_dealRemainingCommunityCards() {
    // 根据当前轮次和设定的结束轮次，发完所有剩余的公共牌
    // 这确保在游戏提前结束时有完整的5张公共牌进行比牌


    if (currentRound == PREFLOP) {
        // 从翻牌前直接结束，需要发完所有公共牌：FLOP + TURN + RIVER
        _dealFlop();
        _dealTurn();
        _dealRiver();
    } else if (currentRound == FLOP) {
        // 从翻牌轮结束，需要发完TURN + RIVER
        _dealTurn();
        _dealRiver();
    } else if (currentRound == TURN) {
        // 从转牌轮结束，只需要发RIVER
        _dealRiver();
    }
    // 如果当前轮次是RIVER，不需要发任何牌

    // 发完公共牌后更新所有玩家的手牌强度和潜力
    _updateHandPotentialForAllPlayers();
}

void PokerEnv::_postAntes() {
    if (ANTE > 0) {
        for (auto p : players) {
            if (p->stack > 0) { // Only if player has chips
                int anteAmount = std::min(ANTE, p->stack);
                p->betRaise(p->currentBet + anteAmount); // betRaise handles stack update and all-in
                p->hasActed = false; // Posting ante is not a voluntary action
            }
        }
    }
}

void PokerEnv::_postSmallBlind() {
    PokerPlayer* sbPlayer = players[sbPos];
    if (sbPlayer->stack > 0) {
        int sbAmount = std::min(SMALL_BLIND, sbPlayer->stack);
        sbPlayer->betRaise(sbPlayer->currentBet + sbAmount);
        sbPlayer->hasActed = false;
    }
}

void PokerEnv::_postBigBlind() {
    PokerPlayer* bbPlayer = players[bbPos];
    if (bbPlayer->stack > 0) {
        int bbAmount = std::min(BIG_BLIND, bbPlayer->stack);
        bbPlayer->betRaise(bbPlayer->currentBet + bbAmount);
        bbPlayer->hasActed = false;
    }
}

bool PokerEnv::_isBettingDone() {
    int numActivePlayers = 0; // Not folded, has chips (not all-in yet or can still act)
    int numActionClosedPlayers = 0; // Acted or All-in
    int maxBetOnTable = 0;

    for (const auto* p : players) {
        if (!p->folded) {
             maxBetOnTable = std::max(maxBetOnTable, p->currentBet);
            if (p->stack > 0 && !p->isAllin) { // Has chips and not already all-in
                numActivePlayers++;
                if (p->hasActed) {
                    numActionClosedPlayers++;
                }
            } else { // Folded or All-in (or stack == 0)
                 numActionClosedPlayers++; // All-in players have effectively "acted" for future bets
            }
        }
    }

    if (getNumActivePlayersNotFolded() <= 1 && currentRound > PREFLOP && nActionsThisEpisode > 0) { // if only one player can act, betting is done (unless it's preflop BB option)
        bool allOthersAllinOrFolded = true;
        int potentiallyActingPlayer = -1;
        int countCanAct = 0;
        for(const auto* p : players) {
            if (!p->folded) {
                if (!p->isAllin && p->stack > 0) {
                    countCanAct++;
                    potentiallyActingPlayer = p->seatId;
                }
            }
        }
        if (countCanAct <=1) return true;
    }


    // Check if all players who are not folded and not all-in have acted AND have the same currentBet amount
    // OR if they are the last raiser and action has come back to them without a re-raise.
    int playersYetToAct = 0;
    for (const auto* p : players) {
        if (!p->folded && !p->isAllin && p->stack > 0) {
            if (!p->hasActed || (p->currentBet < maxBetOnTable && p->seatId != lastRaiser)) {
                 // Special case: BB preflop can act even if currentBet == maxBet if no raise yet
                if (currentRound == PREFLOP && p->seatId == bbPos && maxBetOnTable == BIG_BLIND && lastRaiser == -1 && !p->hasActed) {
                    // BB has option
                    playersYetToAct++;
                } else if (p->currentBet < maxBetOnTable) {
                    // Player needs to call or fold
                    playersYetToAct++;
                } else if (!p->hasActed) {
                    // Player hasn't acted this round yet
                    playersYetToAct++;
                }
            }
        }
    }
    return playersYetToAct == 0;
}

bool PokerEnv::_isHandDone() {
    // 首先检查handIsOver标志（由_moveToNextRound设置）
    if (handIsOver) return true;

    int numNotFolded = 0;
    for (const auto* p : players) {
        if (!p->folded) numNotFolded++;
    }
    if (numNotFolded <= 1) return true;

    // Check if all remaining players are all-in
    int numNotFoldedAndCanBet = 0;
    for (const auto* p : players) {
        if (!p->folded && !p->isAllin && p->stack > 0) {
            numNotFoldedAndCanBet++;
        }
    }
    // If betting is done, and <=1 player can still make betting decisions, the hand might proceed to showdown / next street directly
    if (numNotFoldedAndCanBet <=1 && _isBettingDone()) {
         if (currentRound < RIVER) { // 如果还没到RIVER，但只剩下all-in玩家
             bool allNonFoldedAreAllIn = true;
             for(const auto* p : players) {
                 if(!p->folded && !p->isAllin && p->stack > 0) {
                    allNonFoldedAreAllIn = false;
                    break;
                 }
             }
             if(allNonFoldedAreAllIn) return true; // All remaining are all-in, hand goes to showdown after dealing remaining cards
         }
    }

    return false;
}

void PokerEnv::_moveToNextRound() {

    _putCurrentBetsIntoMainPotAndSidePots();
    _calculateSidePots(); // This should correctly set player sidePotRanks

    if (currentRound < end_with_round) {
        currentRound = ROUND_AFTER[currentRound];
        _dealNextRound();
        for (auto p : players) {
            if (!p->folded) p->hasActed = false; // Reset for players still in hand
            p->investedThisRound = 0.0f;
        }
        currentPlayer = _getFirstToActPostFlop();
        nRaisesThisRound = 0;
        lastRaiser = -1;
        cappedRaise_member.reset();
    } else {
        // 游戏达到了设定的结束轮次
#ifdef  DEBUG_POKER_ENV
        std::cout << "_moveToNextRound: Game reached end_with_round" << std::endl;
#endif
        if (end_with_round == 0) {
            // 如果只玩翻前，直接结束游戏，不发公共牌
#ifdef  DEBUG_POKER_ENV
            std::cout << "_moveToNextRound: end_with_round is 0, ending game without dealing community cards" << std::endl;
#endif
            handIsOver = true;
        } else {
            // 其他情况需要发完剩余的公共牌进行比牌
#ifdef  DEBUG_POKER_ENV
            std::cout << "_moveToNextRound: Dealing remaining community cards for showdown" << std::endl;
#endif
            _dealRemainingCommunityCards();
            handIsOver = true;
        }

    }
}

void PokerEnv::_putCurrentBetsIntoMainPotAndSidePots() {
    for (auto p : players) {
        if (p->currentBet > 0) {
            mainPot += p->currentBet;
            // p->totalInvestedThisHand was already updated by betRaise/checkCall
            p->currentBet = 0;
        }
    }
    // sidePots are calculated from totalInvestedThisHand later in _calculateSidePots
}

void PokerEnv::_calculateSidePots() {
    // This implementation is complex. It needs to mirror PokerEnv.cpp's _calculateSidePots
    // which correctly determines main pot, side pots, and player eligibility (sidePotRank)
    // Based on sorted player investments (totalInvestedThisHand).

    // Simplified version for now, real version from PokerEnv.cpp needed.
    // The crucial part is correctly setting player->sidePotRank.
    // 0 = main pot, 1 = side pot 0, 2 = side pot 1 etc.

    // For now, assume mainPot contains everything and sidePots are empty.
    // This is incorrect for proper side pot logic. A full port is required.
    // The version in PokerEnv.cpp correctly uses PlayerInvestmentInfo.
    // For demonstration, this will be left as a TODO for full porting.

    //--- Start of Ported _calculateSidePots from PokerEnv.cpp (needs PokerPlayer to have totalInvestedThisHand and sidePotRank)
    int totalPotFromBets = this->mainPot; // mainPot currently holds sum of all currentBets + previous main/side pots
    this->mainPot = 0;
    this->sidePots.clear();

    struct PlayerInvestmentInfo {
        PokerPlayer* player;
        int invested; // totalInvestedThisHand
        bool operator<(const PlayerInvestmentInfo& other) const {
            return invested < other.invested;
        }
    };

    std::vector<PlayerInvestmentInfo> player_investments;
    for (PokerPlayer* p : players) {
        if (p && p->totalInvestedThisHand > 0) {
            player_investments.push_back({p, p->totalInvestedThisHand});
        }
    }

    if (player_investments.empty()) {
        this->mainPot = totalPotFromBets; // Should be 0 if no one invested
        return;
    }

    std::sort(player_investments.begin(), player_investments.end());

    int last_bet_level = 0;
    for (const auto& inv_info : player_investments) {
        int current_bet_level = inv_info.invested;
        if (current_bet_level <= last_bet_level) continue;

        int pot_this_layer = 0;
        int layer_contribution_per_player = current_bet_level - last_bet_level;

        for (PokerPlayer* p_contributor : players) { // Iterate all players to check contribution
            if (p_contributor && p_contributor->totalInvestedThisHand >= current_bet_level) {
                pot_this_layer += layer_contribution_per_player;
                // Set sidePotRank: index of the last pot they are eligible for.
                // Main pot: rank 0. Side pot 0 (this->sidePots[0]): rank 1, etc.
                p_contributor->sidePotRank = (this->mainPot == 0 && this->sidePots.empty()) ? 0 : 1 + static_cast<int>(this->sidePots.size());
            } else if (p_contributor && p_contributor->totalInvestedThisHand > last_bet_level && p_contributor->totalInvestedThisHand < current_bet_level) {
                // Player is all-in at a level below current_bet_level but above last_bet_level
                 pot_this_layer += (p_contributor->totalInvestedThisHand - last_bet_level);
                 p_contributor->sidePotRank = (this->mainPot == 0 && this->sidePots.empty()) ? 0 : 1 + static_cast<int>(this->sidePots.size());
            }
        }


        if (pot_this_layer > 0) {
            if (this->mainPot == 0 && this->sidePots.empty()) { // First pot is main pot
                this->mainPot = pot_this_layer;
            } else {
                this->sidePots.push_back(pot_this_layer);
            }
        }
        last_bet_level = current_bet_level;
    }
    //--- End of Ported _calculateSidePots
}

void PokerEnv::_calculateCurrentSidePots() {
    // 实时计算当前边池状态，基于玩家的totalInvestedThisHand + currentBet
    // 这个方法在每次下注后立即调用，提供实时的边池信息

    // 临时存储：计算虚拟的边池，但不改变实际的mainPot和sidePots
    // 用于观察向量，实际的边池分配仍在_assignRewardsAndResetBets中进行

    struct PlayerInvestmentInfo {
        PokerPlayer* player;
        int invested; // totalInvestedThisHand + currentBet
        bool operator<(const PlayerInvestmentInfo& other) const {
            return invested < other.invested;
        }
    };

    std::vector<PlayerInvestmentInfo> player_investments;
    for (PokerPlayer* p : players) {
        if (p && (p->totalInvestedThisHand + p->currentBet) > 0) {
            player_investments.push_back({p, p->totalInvestedThisHand + p->currentBet});
        }
    }

    if (player_investments.empty()) {
        // 清空当前边池信息
        currentMainPot = mainPot;
        currentSidePots.clear();
        currentSidePots.resize(N_SEATS, 0);
        for (PokerPlayer* p : players) {
            if (p) p->currentSidePotRank = 0;
        }
        return;
    }

    std::sort(player_investments.begin(), player_investments.end());

    // 重置当前边池信息
    currentMainPot = mainPot; // 已经收集的主池
    currentSidePots.clear();
    currentSidePots.resize(N_SEATS, 0);

    int last_bet_level = 0;
    for (const auto& inv_info : player_investments) {
        int current_bet_level = inv_info.invested;
        if (current_bet_level <= last_bet_level) continue;

        int pot_this_layer = 0;
        int layer_contribution_per_player = current_bet_level - last_bet_level;

        for (PokerPlayer* p_contributor : players) {
            if (p_contributor && (p_contributor->totalInvestedThisHand + p_contributor->currentBet) >= current_bet_level) {
                pot_this_layer += layer_contribution_per_player;
                // 设置当前边池等级
                p_contributor->currentSidePotRank = (currentMainPot == mainPot && currentSidePots.empty()) ? 0 : 1 + static_cast<int>(currentSidePots.size());
            } else if (p_contributor && (p_contributor->totalInvestedThisHand + p_contributor->currentBet) > last_bet_level &&
                      (p_contributor->totalInvestedThisHand + p_contributor->currentBet) < current_bet_level) {
                pot_this_layer += ((p_contributor->totalInvestedThisHand + p_contributor->currentBet) - last_bet_level);
                p_contributor->currentSidePotRank = (currentMainPot == mainPot && currentSidePots.empty()) ? 0 : 1 + static_cast<int>(currentSidePots.size());
            }
        }

        if (pot_this_layer > 0) {
            if (currentMainPot == mainPot && currentSidePots.empty()) {
                currentMainPot += pot_this_layer;
            } else {
                if (currentSidePots.size() < N_SEATS) { // Check bounds before access
                    currentSidePots[currentSidePots.size()] = pot_this_layer; // This logic is wrong for pushing to vector.
                                                                            // sidePots.push_back(pot_this_layer) if it's a std::vector
                                                                            // This currentSidePots is resized to N_SEATS.
                                                                            // It should be:
                                                                            // currentSidePots.push_back(pot_this_layer);
                                                                            // Or if it's meant to be an array:
                                                                            // if (idx < N_SEATS) currentSidePots[idx++] = pot_this_layer
                                                                            // For now, assuming it tries to act like a list of side pots:
                                                                            // This should likely be currentSidePots.push_back(pot_this_layer)
                                                                            // and currentSidePots should not be pre-resized with 0s if it's dynamic.
                                                                            // Given it's used as currentSidePots[i] in obs, it's an array.
                                                                            // Let's assume the original intent was to fill it sequentially:
                    int current_side_pot_idx = 0;
                    while(current_side_pot_idx < currentSidePots.size() && currentSidePots[current_side_pot_idx] != 0) {
                        current_side_pot_idx++;
                    }
                    if (current_side_pot_idx < currentSidePots.size()) {
                         currentSidePots[current_side_pot_idx] = pot_this_layer;
                    }
                }
            }
        }
        last_bet_level = current_bet_level;
    }
}

int PokerEnv::_getCurrentTotalMinRaise() {
    // Ported from PokerEnv.cpp
    if (N_SEATS == 0) return BIG_BLIND;
    int maxBet = 0;
    int secondMaxBet = 0;
    for(const auto* p : players) {
        if (p->currentBet > maxBet) {
            secondMaxBet = maxBet;
            maxBet = p->currentBet;
        } else if (p->currentBet > secondMaxBet && p->currentBet < maxBet) {
            secondMaxBet = p->currentBet;
        }
    }
    int lastRaiseSize = maxBet - secondMaxBet;
    return maxBet + std::max(lastRaiseSize, BIG_BLIND);
}

int PokerEnv::getFractionOfPotRaise(float fraction, PokerPlayer* playerThatBets) {
    // Ported from PokerEnv.cpp
    if (!playerThatBets) return 0;

    int biggestBetOutThere = _getBiggestBetOutThereAkaTotalToCall();
    int toCall = biggestBetOutThere - playerThatBets->currentBet;
    toCall = std::max(0, toCall);

    int potBeforeAction = mainPot; // Current main pot
    for (int sp : sidePots) potBeforeAction += sp; // Plus side pots
    for (const auto* p : players) potBeforeAction += p->currentBet; // Plus bets on table

    int potAfterCall = potBeforeAction + toCall;
    int delta = static_cast<int>(static_cast<float>(toCall) + (static_cast<float>(potAfterCall) * fraction));
    int totalRaise = delta + playerThatBets->currentBet; // This is target total bet for playerThatBets

    // Clamping / min raise adjustment should happen in _processRaise or _getFixedAction
    return totalRaise;
}


// Base function: getHandRank from vectors of Card pointers
// This is one of the top-level functions that directly calls phevaluator logic.
int PokerEnv::getHandRank(const std::vector<Card*>& hand_cards, const std::vector<Card*>& board_cards) const {
    std::vector<Card*> all_cards = hand_cards;
    all_cards.insert(all_cards.end(), board_cards.begin(), board_cards.end());

    // Filter out nullptrs that might have been passed
    all_cards.erase(std::remove(all_cards.begin(), all_cards.end(), nullptr), all_cards.end());

    // Convert to phevaluator's int representation
    std::vector<int> eval_cards_int;
    eval_cards_int.reserve(all_cards.size());
    for(Card* c : all_cards) {
        // No need to check for nullptr here if already removed, but double check for safety
        if (c) {
            eval_cards_int.push_back(_convert_card_to_phevaluator_int(c));
        }
    }

    // Filter out any -1 (error indicators from conversion if any were pushed)
    eval_cards_int.erase(std::remove(eval_cards_int.begin(), eval_cards_int.end(), -1), eval_cards_int.end());

    if (eval_cards_int.size() < 5) {
        return 0; // Not enough cards to evaluate
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRank(vec_card*, vec_card*)] Input hand_cards (" << hand_cards.size() << "), board_cards (" << board_cards.size() << ")" << std::endl;
    std::cout << "  eval_cards_int for phevaluator (size " << eval_cards_int.size() << "): ";
    for (size_t i = 0; i < eval_cards_int.size(); ++i) {
        std::cout << eval_cards_int[i] << (i == eval_cards_int.size() - 1 ? "" : ", ");
    }
    std::cout << std::endl;
    std::cout << "  eval_cards_int breakdown (pheval_rank, pheval_suit): ";
    for (int card_int : eval_cards_int) {
        if (card_int != -1) {
             std::cout << "(" << (card_int / 4) << "," << (card_int % 4) << ") ";
        } else {
             std::cout << "(-1) ";
        }
    }
    std::cout << std::endl;
    #endif

    // Call the appropriate phevaluator function based on the number of cards
    int rank_val = 7463; // Default to phevaluator's worst rank representation + 1
    if (eval_cards_int.size() == 5) {
        rank_val = evaluate_5cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4]);
    } else if (eval_cards_int.size() == 6) {
        rank_val = evaluate_6cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5]);
    } else if (eval_cards_int.size() == 7) { // Max 7 cards
        rank_val = evaluate_7cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5], eval_cards_int[6]);
    } else if (eval_cards_int.size() > 7) {
         #ifdef DEBUG_POKER_ENV
        std::cout << "[WARNING PokerEnv::getHandRank(vec_card*, vec_card*)] More than 7 cards after processing (" << eval_cards_int.size() << "). Evaluating first 7." << std::endl;
        #endif
        rank_val = evaluate_7cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5], eval_cards_int[6]);
    }
    // phevaluator: 1 is best (Royal/Straight Flush), 7462 is worst (High Card).
    // Return phevaluator result directly: 1 = best, 7462 = worst
    // Ensure 0 from pheval (error/no rank) remains 0. (pheval returns 0 for error if input is bad, e.g. duplicate cards)
    return rank_val == 0 ? 0 : rank_val;
}

void PokerEnv::_assignRewardsAndResetBets() {
    _calculateSidePots(); // Ensure pots are correctly structured first

    lastHandWinnings.clear();
    std::vector<PokerPlayer*> playersToShowHands;
    for(PokerPlayer* p : players) {
        // Eligible for showdown if not folded OR folded but was all-in
        if (p && (!p->folded || p->isAllin) && p->totalInvestedThisHand > 0) {
            playersToShowHands.push_back(p);
        }
    }

    // If only one player remains eligible for any pot money (others folded without all-in)
    if (playersToShowHands.size() == 1) {
        PokerPlayer* winner = playersToShowHands[0];
        int totalWinnings = 0;
        std::string handDesc = "Won by Default";
         if (!winner->folded) { // If they didn't fold, get hand description
            int rank = getHandRank(winner->hand, getCommunityCards());
            handDesc = getHandDescriptionFromRank(rank); // Pass phevaluator rank directly
         }

        if (mainPot > 0) {
            winner->award(mainPot);
            totalWinnings += mainPot;
            lastHandWinnings.push_back({winner->seatId, mainPot, "Main Pot", handDesc, winner->hand});
            mainPot = 0;
        }
        for (size_t i = 0; i < sidePots.size(); ++i) {
            if (sidePots[i] > 0) {
                 // Check eligibility for this side pot (based on sidePotRank)
                 // Player rank N is eligible for pots 0 to N. Main=0, SP0=1, SP1=2...
                 if (winner->sidePotRank >= static_cast<int>(i + 1)) {
                    winner->award(sidePots[i]);
                    totalWinnings += sidePots[i];
                    lastHandWinnings.push_back({winner->seatId, sidePots[i], "Side Pot " + std::to_string(i + 1), handDesc, winner->hand});
                    sidePots[i] = 0;
                 }
            }
        }
    } else if (playersToShowHands.size() > 1) {
        // --- Distribute Main Pot ---
        std::vector<PokerPlayer*> mainPotContenders;
        for(PokerPlayer* p : playersToShowHands) {
            if (p->sidePotRank >= 0) mainPotContenders.push_back(p); // Rank 0 means main pot
        }
        if (mainPot > 0 && !mainPotContenders.empty()) {
            distributePot(mainPot, mainPotContenders, "Main Pot");
        }
        mainPot = 0;

        // --- Distribute Side Pots ---
        for (size_t i = 0; i < sidePots.size(); ++i) {
            if (sidePots[i] > 0) {
                std::vector<PokerPlayer*> sidePotContenders;
                for (PokerPlayer* p : playersToShowHands) {
                    // Player rank N is eligible for pots 0..N. sidePots[i] is pot number i+1.
                    if (p->sidePotRank >= static_cast<int>(i + 1)) {
                        sidePotContenders.push_back(p);
                    }
                }
                if (!sidePotContenders.empty()) {
                    distributePot(sidePots[i], sidePotContenders, "Side Pot " + std::to_string(i + 1));
                }
                sidePots[i] = 0;
            }
        }
    }
    // Else (no one eligible for pots, e.g. error or all folded with no investment for some reason)

    // Player state reset (currentBet, totalInvested) is handled by PokerPlayer::reset called by PokerEnv::reset
}

void PokerEnv::distributePot(int potAmount, std::vector<PokerPlayer*>& contenders, const std::string& potName) {
    if (potAmount <= 0 || contenders.empty()) return;

    // 过滤出参与摊牌的玩家（没有弃牌的，或者全下的玩家）
    std::vector<PokerPlayer*> showdownPlayers;
    for(PokerPlayer* p : contenders) {
        if (!p->folded || p->isAllin) {
            showdownPlayers.push_back(p);
        }
    }

    if (showdownPlayers.empty()) return; // 没有人参与摊牌

    // 如果只有一个玩家，直接获胜
    if (showdownPlayers.size() == 1) {
        PokerPlayer* winner = showdownPlayers[0];
        winner->award(potAmount);
        lastHandWinnings.push_back({
            winner->seatId,
            potAmount,
            potName,
            "Won by default",
            winner->hand
        });
        return;
    }

    // 使用新的牌力分组功能
    std::vector<std::vector<PokerPlayer*>> handGroups = groupPlayersByHandStrength(showdownPlayers);

    if (handGroups.empty()) return;

    // 最强的一组获胜者（第一组就是最强的）
    std::vector<PokerPlayer*> winners = handGroups[0];

    if (!winners.empty()) {
        // 计算每个获胜者应该得到的金额
        int prizePerWinner = potAmount / winners.size();
        int remainder = potAmount % winners.size();

        // 获取获胜手牌的描述
        int bestRank = getHandRank(winners[0]->hand, communityCards);
        std::string handDesc = getHandDescriptionFromRank(bestRank);

        // 分配奖金给获胜者
        for (size_t i = 0; i < winners.size(); ++i) {
            int finalPrize = prizePerWinner;
            if (i == 0) {
                finalPrize += remainder; // 余数给第一个获胜者
            }

            winners[i]->award(finalPrize);
            lastHandWinnings.push_back({
                winners[i]->seatId,
                finalPrize,
                potName,
                handDesc,
                winners[i]->hand
            });
        }
    }
}

int PokerEnv::_adjustRaise(float raiseTotalAmountInChips_float) {
    // This needs to implement the full logic from PokerEnv.cpp version
    // including fixed limit, pot limit, no limit rules.
    // For No-Limit, it's clamping to player's stack. Min raise is handled before.
    int raiseTo = static_cast<int>(raiseTotalAmountInChips_float);

    // Basic No-Limit clamping: cannot bet more than you have
    // (This is handled by PokerPlayer::betRaise generally)
    // Minimum raise size is handled by _getCurrentTotalMinRaise and subsequent checks

    // If IS_FIXED_LIMIT_GAME, raise amount is fixed per round.
    // If IS_POT_LIMIT_GAME, raise amount is capped by pot size.

    // This is a simplified placeholder for No-Limit.
    return raiseTo;
}


// --- Getters and other public methods ---
int PokerEnv::getNumPlayers() const { return N_SEATS; }
const std::vector<Card*>& PokerEnv::getCommunityCards() const { return communityCards; }

int PokerEnv::getPotSize() const {
    // 返回真正的当前底池：已收集的mainPot + sidePots + 桌上的currentBet
    int total_pot = mainPot + std::accumulate(sidePots.begin(), sidePots.end(), 0);

    // 加上桌上的下注
    for (const PokerPlayer* p : players) {
        if (p) {
            total_pot += p->currentBet;
        }
    }

    return total_pot;
}

int PokerEnv::getCurrentBet() const { // This is effectively _getBiggestBetOutThereAkaTotalToCall
    return _getBiggestBetOutThereAkaTotalToCall();
}
float PokerEnv::getRewardScalar() const { return REWARD_SCALAR; }


int PokerEnv::_getFirstToActPreFlop() {
    if (N_SEATS == 0) return -1;

    int firstToAct;

    // 如果设置了fix_utg_position参数且值有效，则固定UTG位置
    if (fix_utg_position >= 0 && fix_utg_position < N_SEATS) {
        firstToAct = fix_utg_position;
    } else {
        // 否则使用默认逻辑：BB后一位
        firstToAct = (bbPos + 1) % N_SEATS;
    }

    // Find first player from this pos who is not folded and not all-in
    for (int i=0; i<N_SEATS; ++i) {
        int check_idx = (firstToAct + i) % N_SEATS;
        if (!players[check_idx]->folded && !players[check_idx]->isAllin && players[check_idx]->stack > 0) return check_idx;
    }
    return firstToAct; // Fallback if all are folded/all-in (should be caught by isHandDone)
}

int PokerEnv::_getFirstToActPostFlop() {
    // 翻牌后第一个行动的是小盲注位或者按钮位后的第一个未弃牌玩家
    int numPlayers = players.size();
    if (numPlayers == 0) return -1; // No players

    int firstToActInitial;

    // 如果设置了fix_utg_position且当前轮次为翻牌后，仍需保持相同的位置关系
    // 但在翻牌后，行动顺序通常从SB开始，而不是UTG
    if (fix_utg_position >= 0 && fix_utg_position < N_SEATS) {
        // 当固定UTG时，翻牌后仍从SB位开始行动
        firstToActInitial = sbPos;
    } else {
        // 使用默认逻辑
        firstToActInitial = (buttonPos + 1) % numPlayers;
        if (N_SEATS == 2) { // Heads-up: button is SB and acts first post-flop
            firstToActInitial = buttonPos;
        }
    }

    // 寻找第一个未弃牌且未全下的玩家
    for (int i = 0; i < numPlayers; ++i) {
        int pos = (firstToActInitial + i) % numPlayers;
        // Ensure player at 'pos' exists before dereferencing
        if (players[pos] && !players[pos]->folded && !players[pos]->isAllin) {
            return pos;
        }
    }

    // 如果所有人都弃牌或全下，返回初始计算位置
    return firstToActInitial;
}



void PokerEnv::printBoard() {
    for(auto c : communityCards) if(c) std::cout << c->toString() << " ";
    std::cout << std::endl;
}

void PokerEnv::printHands() {
    for(auto p : players) {
        std::cout << "P" << p->seatId << ": ";
        p->printHand();
        std::cout << " S:" << p->stack << " B:" << p->currentBet << std::endl;
    }
}

nlohmann::json PokerEnv::state_dict() const {
    nlohmann::json state;
    state["args_config"] = args_config; // Save original config
    state["N_SEATS"] = N_SEATS;
    state["SMALL_BLIND"] = SMALL_BLIND;
    state["BIG_BLIND"] = BIG_BLIND;
    state["ANTE"] = ANTE;
    state["DEFAULT_STACK_SIZE"] = DEFAULT_STACK_SIZE;
    state["REWARD_SCALAR"] = REWARD_SCALAR;
    state["N_ACTIONS"] = N_ACTIONS;
    state["IS_EVALUATING"] = IS_EVALUATING;

    state["buttonPos"] = buttonPos;
    state["sbPos"] = sbPos;
    state["bbPos"] = bbPos;
    state["currentPlayer"] = currentPlayer;
    state["currentRound"] = currentRound;
    state["mainPot"] = mainPot;
    state["sidePots"] = sidePots;
    state["handIsOver"] = handIsOver;
    state["betSizesListAsFracOfPot"] = betSizesListAsFracOfPot;
    state["uniformActionInterpolation_member"] = uniformActionInterpolation_member;
    state["lastAction_member"] = lastAction_member;
    state["lastRaiser"] = lastRaiser;
    state["nRaisesThisRound"] = nRaisesThisRound;
    state["nActionsThisEpisode"] = nActionsThisEpisode;

    nlohmann::json capped_raise_json;
    capped_raise_json["happenedThisRound"] = cappedRaise_member.happenedThisRound;
    capped_raise_json["playerThatRaised"] = cappedRaise_member.playerThatRaised;
    capped_raise_json["playerThatCantReopen"] = cappedRaise_member.playerThatCantReopen;
    state["cappedRaise_member"] = capped_raise_json;

    state["FIRST_ACTION_NO_CALL"] = FIRST_ACTION_NO_CALL;
    state["IS_FIXED_LIMIT_GAME"] = IS_FIXED_LIMIT_GAME;
    state["MAX_N_RAISES_PER_ROUND"] = MAX_N_RAISES_PER_ROUND;
    state["fix_utg_position"] = fix_utg_position;

    nlohmann::json players_json = nlohmann::json::array();
    for(const auto* p : players) players_json.push_back(p->state_dict());
    state["players"] = players_json;

    nlohmann::json community_cards_json = nlohmann::json::array();
    for(const Card* c : communityCards) {
        if (c) {
            // 手动创建数组而不是使用tuple
            nlohmann::json card_array = nlohmann::json::array();
            card_array.push_back(static_cast<int>(c->getValue()));
            card_array.push_back(static_cast<int>(c->getSuit()));
            community_cards_json.push_back(card_array);
        } else {
            community_cards_json.push_back(nullptr); // For empty board slots
        }
    }
    state["communityCards"] = community_cards_json;

    state["deck"] = deck->state_dict(); // Assuming Deck has state_dict

    nlohmann::json last_winnings_json = nlohmann::json::array();
    for(const auto& lw : lastHandWinnings) {
        nlohmann::json lw_item;
        lw_item["seatId"] = lw.seatId;
        lw_item["amountWon"] = lw.amountWon;
        lw_item["potDescription"] = lw.potDescription;
        lw_item["handDescription"] = lw.handDescription;
        nlohmann::json hc_json = nlohmann::json::array();
        for(const Card* card : lw.holeCards) {
            if (card) {
                // 手动创建数组而不是使用tuple
                nlohmann::json card_array = nlohmann::json::array();
                card_array.push_back(static_cast<int>(card->getValue()));
                card_array.push_back(static_cast<int>(card->getSuit()));
                hc_json.push_back(card_array);
            }
        }
        lw_item["holeCards"] = hc_json;
        last_winnings_json.push_back(lw_item);
    }
    state["lastHandWinnings"] = last_winnings_json;

    // m_rng state is tricky, usually not serialized directly like this for mt19937
    return state;
}

void PokerEnv::load_state_dict(const nlohmann::json& state, bool blank_private_info) {
    // args_config = state["args_config"]; // Careful with overwriting if ctor uses it
    // 优先使用传入的 state 中的 args_config，如果存在的话
    if (state.contains("args_config")) {
        args_config = state["args_config"];
    }

    N_SEATS = state["N_SEATS"]; // Should match constructor if not dynamic
    SMALL_BLIND = state["SMALL_BLIND"];
    BIG_BLIND = state["BIG_BLIND"];
    ANTE = state["ANTE"];
    DEFAULT_STACK_SIZE = state["DEFAULT_STACK_SIZE"];
    REWARD_SCALAR = state["REWARD_SCALAR"];
    N_ACTIONS = state["N_ACTIONS"];
    IS_EVALUATING = state["IS_EVALUATING"];

    buttonPos = state["buttonPos"];
    sbPos = state["sbPos"];
    bbPos = state["bbPos"];
    currentPlayer = state["currentPlayer"];
    currentRound = state["currentRound"];
    mainPot = state["mainPot"];
    sidePots = state["sidePots"].get<std::vector<int>>();
    handIsOver = state["handIsOver"];
    betSizesListAsFracOfPot = state["betSizesListAsFracOfPot"].get<std::vector<float>>();
    uniformActionInterpolation_member = state["uniformActionInterpolation_member"];
    lastAction_member = state["lastAction_member"].get<std::vector<int>>();
    lastRaiser = state["lastRaiser"];
    nRaisesThisRound = state["nRaisesThisRound"];
    nActionsThisEpisode = state["nActionsThisEpisode"];

    if (state.contains("cappedRaise_member")) {
        const auto& cr_json = state["cappedRaise_member"];
        cappedRaise_member.happenedThisRound = cr_json["happenedThisRound"];
        cappedRaise_member.playerThatRaised = cr_json["playerThatRaised"];
        cappedRaise_member.playerThatCantReopen = cr_json["playerThatCantReopen"];
    }

    FIRST_ACTION_NO_CALL = state.value("FIRST_ACTION_NO_CALL", false);
    IS_FIXED_LIMIT_GAME = state.value("IS_FIXED_LIMIT_GAME", false);
    if (state.contains("MAX_N_RAISES_PER_ROUND")) {
        MAX_N_RAISES_PER_ROUND = state["MAX_N_RAISES_PER_ROUND"].get<std::vector<int>>();
    }
    fix_utg_position = state.value("fix_utg_position", -1);


    const auto& players_json = state["players"];
    // Ensure players vector is sized correctly or re-created
    if (players.size() != static_cast<size_t>(N_SEATS)) { // Use static_cast for comparison
        for(auto p : players) delete p;
        players.resize(N_SEATS);
        for(int i=0; i<N_SEATS; ++i) players[i] = new PokerPlayer(i,0); // Temp stack
    }
    for(size_t i=0; i<players_json.size(); ++i) {
        players[i]->load_state_dict(players_json[i], blank_private_info);
    }

    deck->load_state_dict(state["deck"]); //Requires Deck to implement this

    const auto& comm_cards_json = state["communityCards"];
    communityCards.assign(N_COMMUNITY_CARDS, nullptr); // Clear and resize
    for(size_t i=0; i<comm_cards_json.size(); ++i) {
        if (!comm_cards_json[i].is_null()) {
            // Find card from deck - this is critical. Deck must provide this.
            // Or, Card objects are created here, but Deck might lose track / double-delete.
            // Placeholder: Assume deck can find/provide card pointer
            // communityCards[i] = deck->findCard(comm_cards_json[i][0].get<int>(), comm_cards_json[i][1].get<int>());
        }
    }

    lastHandWinnings.clear();
    if(state.contains("lastHandWinnings")) {
        for(const auto& lw_json : state["lastHandWinnings"]) {
            PlayerWinningInfo lw_info;
            lw_info.seatId = lw_json["seatId"];
            lw_info.amountWon = lw_json["amountWon"];
            lw_info.potDescription = lw_json["potDescription"];
            lw_info.handDescription = lw_json["handDescription"];
            // Hole cards also need recovery from deck or careful reconstruction
            lastHandWinnings.push_back(lw_info);
        }
    }
    // RNG state typically not loaded this way, would need specific mt19937 serialization.
    _initPrivObsLookUp(); // When state is loaded, esp. if args_config (and thus suits_matter) might change.
    _initRangeIdxLut(); // When state is loaded, also initialize the range_idx LUT.
}

int PokerEnv::findNextPlayerToAct(int current_player_idx) {
    if (N_SEATS == 0) return -1;
    for (int i = 1; i <= N_SEATS; ++i) {
        int next_idx = (current_player_idx + i) % N_SEATS;
        if (players[next_idx] && !players[next_idx]->folded && !players[next_idx]->isAllin && players[next_idx]->stack > 0) {
            return next_idx;
        }
    }
    return -1; // No one can act
}

int PokerEnv::_getMinValidRaise(const PokerPlayer* player, int maxBetOnTable) const {
    // Simplified from PokerEnv.cpp. Full logic depends on last raise size.
    int minRaiseDelta = BIG_BLIND; // Default min raise increase is BB
    // More accurately, it's max(BIG_BLIND, last_raise_amount_delta)
    // For now, using BIG_BLIND as the minimum increment.
    return maxBetOnTable + minRaiseDelta;
}

const std::vector<PlayerWinningInfo>& PokerEnv::getLastHandWinnings() const {
    return lastHandWinnings;
}

int PokerEnv::_getMinValidBet(const PokerPlayer* player) const {
    return BIG_BLIND; // Minimum opening bet is BB
}

int PokerEnv::_getBiggestBetOutThereAkaTotalToCall() const {
    int maxBet = 0;
    for (const auto* p : players) {
        if (p) maxBet = std::max(maxBet, p->currentBet);
    }
    return maxBet;
}

int PokerEnv::getNumActivePlayersNotFolded() const {
    int count = 0;
    for (const auto* p : players) {
        if (p && !p->folded) count++;
    }
    return count;
}


// Python binding friendly methods
std::vector<std::vector<float>> PokerEnv::getPublicObservation_py() {
    return getPublicObservation();
}

int64_t PokerEnv::getRangeIdx_py(int playerId) { return getRangeIdx(playerId); }
std::vector<float> PokerEnv::getLegalActionMask_py() { return getLegalActionMask(); }

std::tuple<std::vector<std::vector<float>>, std::vector<float>, std::vector<float>, bool> PokerEnv::step_py(int actionInt) {
    return step(actionInt);
}
std::tuple<std::vector<std::vector<float>>, std::vector<float>, std::vector<float>, bool> PokerEnv::step_py(int actionType, float amount) {
    return step(actionType, amount);
}
std::tuple<std::vector<std::vector<float>>, std::vector<float>, std::vector<float>, bool> PokerEnv::step_py_int(int actionInt) {
    // Assuming step_py_int is for the discrete action interface
    return step(actionInt);
}

std::vector<int> PokerEnv::getCommunityCards_py() {
    std::vector<int> cc_ints;
    for (const Card* c : communityCards) {
        if (c) cc_ints.push_back(_cardToInt(c));
    }
    return cc_ints;
}

long PokerEnv::getPlayerStack_py(int playerId) {
    if (playerId >=0 && playerId < N_SEATS && players[playerId]) return static_cast<long>(players[playerId]->stack);
    return 0L;
}

// Implementations for new player-specific getters
int PokerEnv::getPlayerCurrentBet_py(int playerId) const {
    if (playerId >= 0 && playerId < N_SEATS && players[playerId]) {
        return players[playerId]->currentBet;
    }
    return 0; // Default or error value
}

bool PokerEnv::getPlayerFolded_py(int playerId) const {
    if (playerId >= 0 && playerId < N_SEATS && players[playerId]) {
        return players[playerId]->folded;
    }
    return true; // Default or error value (e.g., assume folded if invalid ID)
}

bool PokerEnv::getPlayerIsAllin_py(int playerId) const {
    if (playerId >= 0 && playerId < N_SEATS && players[playerId]) {
        return players[playerId]->isAllin;
    }
    return false; // Default or error value
}

std::vector<std::tuple<int, int>> PokerEnv::getPlayerHand_py(int playerId) const {
    std::vector<std::tuple<int, int>> hand_tuples;
    if (playerId >= 0 && playerId < N_SEATS && players[playerId]) {
        for (const Card* card_ptr : players[playerId]->hand) {
            if (card_ptr) {
                hand_tuples.push_back(_cardToTuple(card_ptr));
            }
        }
    }
    return hand_tuples;
}

std::vector<std::tuple<int, int, int, int, std::vector<std::tuple<int, int>>>> PokerEnv::getLastHandWinnings_py() {
    std::vector<std::tuple<int, int, int, int, std::vector<std::tuple<int, int>>>> winnings_tuples;
    for(const auto& win_info : lastHandWinnings) {
        std::vector<std::tuple<int, int>> hole_cards_tuples;
        for(const Card* card_ptr : win_info.holeCards) {
            if (card_ptr) hole_cards_tuples.push_back(_cardToTuple(card_ptr));
        }
        // Placeholder pot/hand desc idx. In a real system, map strings to fixed ints.
        int pot_desc_idx = (win_info.potDescription == "Main Pot" ? 0 : (win_info.potDescription.find("Side Pot") != std::string::npos ? 1 : 2));
        int hand_desc_idx = 0; // Needs mapping based on win_info.handDescription string
        winnings_tuples.emplace_back(win_info.seatId, win_info.amountWon, pot_desc_idx, hand_desc_idx, hole_cards_tuples);
    }
    return winnings_tuples;
}

float PokerEnv::getRewardScalar_py() { return getRewardScalar(); }

std::vector<float> PokerEnv::get_extra_features_py() const {
    // 获取手牌强度
    float hand_strength = getCurrentPlayerInitialHandStrength();

    // 获取多维度评估并归一化
    auto multi_eval = getCurrentPlayerHandMultidimensional();
    float equity_vs_all_normalized = static_cast<float>(std::get<0>(multi_eval)) / 10000.0f;  // 归一化到 [0, 1]
    float equity_vs_pair_sets_normalized = static_cast<float>(std::get<1>(multi_eval)) / 10000.0f;  // 归一化到 [0, 1]

    // 返回合并的特征向量：[hand_strength, equity_vs_all_normalized, equity_vs_pair_sets_normalized]
    return std::vector<float>{hand_strength, equity_vs_all_normalized, equity_vs_pair_sets_normalized};
}


std::vector<float> PokerEnv::_processCheckCall(int totalToCall) {
    // This needs to be ported from PokerEnv.cpp
    // It calculates the actual amount to call and returns {CHECK_CALL, actual_total_bet_after_call}
    // Placeholder:
    PokerPlayer* player = players[currentPlayer];
    int amount_needed = totalToCall - player->currentBet;
    int actual_call_amount = std::min(amount_needed, player->stack);
    float final_bet_amount = static_cast<float>(player->currentBet + actual_call_amount);
    return {static_cast<float>(CHECK_CALL), final_bet_amount};
}

std::vector<float> PokerEnv::_processRaise(float raiseTotalAmountInChips_float) {
    // This needs to be ported from PokerEnv.cpp
    // It adjusts the raise amount (min/max, all-in) and returns {BET_RAISE, actual_total_bet_after_raise}
    // Placeholder:
    PokerPlayer* player = players[currentPlayer];
    int intended_total_bet = static_cast<int>(raiseTotalAmountInChips_float);

    // Clamp to min raise
    int min_raise_total = _getCurrentTotalMinRaise();
    intended_total_bet = std::max(min_raise_total, intended_total_bet);

    // Clamp to all-in
    intended_total_bet = std::min(intended_total_bet, player->currentBet + player->stack);

    return {static_cast<float>(BET_RAISE), static_cast<float>(intended_total_bet)};
}

std::string PokerEnv::getHandDescriptionFromRank(int phevaluatorRank) const {
    // phevaluatorRank is 1 (best) to 7462 (worst)
    if (phevaluatorRank <= 0 || phevaluatorRank > 7462) return "Invalid Rank";
    if (phevaluatorRank <= 10) return "Straight Flush";
    if (phevaluatorRank <= 166) return "Four of a Kind";
    if (phevaluatorRank <= 322) return "Full House";
    if (phevaluatorRank <= 1599) return "Flush";
    if (phevaluatorRank <= 1609) return "Straight";
    // Cater's ranges: 3K (1610-2467), 2P (2468-3325), 1P (3326-6185), HC (6186-7462)
    if (phevaluatorRank <= 2467) return "Three of a Kind";
    if (phevaluatorRank <= 3325) return "Two Pair";
    if (phevaluatorRank <= 6185) return "One Pair";
    return "High Card";
}

// Static helper for Python bindings (if needed, or direct conversion in bindings)
std::tuple<int, int> PokerEnv::_cardToTuple(const Card* card) {
    if (card) {
        return std::make_tuple(static_cast<int>(card->getValue()), static_cast<int>(card->getSuit()));
    }
    return std::make_tuple(-1, -1); // Invalid card representation
}
int PokerEnv::_cardToInt(const Card* card) {
    if (card) {
        return static_cast<int>(card->getValue()) * 4 + static_cast<int>(card->getSuit());
    }
    return -1; // Invalid card representation
}

// --- Start of _getEnvAdjustedActionFormulation (replacement) ---
std::vector<float> PokerEnv::_getEnvAdjustedActionFormulation(int actionInt) {
    // 将离散动作转换为实际动作
    if (actionInt == FOLD) {
        return {static_cast<float>(FOLD), -1.0f}; // 弃牌
    }

    if (actionInt == CHECK_CALL) {
        return {static_cast<float>(CHECK_CALL), -1.0f}; // 跟注/让牌
    }

    if (actionInt >= 2 && actionInt < N_ACTIONS) {
        // 检查 currentPlayer 是否有效
        if (currentPlayer < 0 || currentPlayer >= N_SEATS || !players[currentPlayer]) {
#ifdef DEBUG_POKER_ENV
            std::cerr << "Error: Invalid current player in _getEnvAdjustedActionFormulation." << std::endl;
#endif
            return {static_cast<float>(CHECK_CALL), -1.0f};
        }

        if (actionInt - 2 < 0 || static_cast<size_t>(actionInt - 2) >= betSizesListAsFracOfPot.size()) {
#ifdef DEBUG_POKER_ENV
            std::cerr << "Error: actionInt out of bounds for betSizesListAsFracOfPot." << std::endl;
#endif
            return {static_cast<float>(CHECK_CALL), -1.0f};
        }
        float fraction = betSizesListAsFracOfPot[actionInt - 2];
        int raiseAmount = getFractionOfPotRaise(fraction, players[currentPlayer]);

        if (uniformActionInterpolation_member && !IS_EVALUATING) {
            int maxAmount;
            if (actionInt == N_ACTIONS - 1) {
                maxAmount = players[currentPlayer]->stack + players[currentPlayer]->currentBet;
            } else {
                 if (actionInt - 1 < 0 || static_cast<size_t>(actionInt - 1) >= betSizesListAsFracOfPot.size()) {
#ifdef DEBUG_POKER_ENV
                     std::cerr << "Error: actionInt-1 out of bounds for betSizesListAsFracOfPot (maxAmount calc)." << std::endl;
#endif
                     maxAmount = raiseAmount;
                } else {
                    float biggerFraction = betSizesListAsFracOfPot[actionInt - 1];
                    int biggerAmount = getFractionOfPotRaise(biggerFraction, players[currentPlayer]);
                    maxAmount = (raiseAmount + biggerAmount) / 2;
                }
            }

            int minAmount;
            if (actionInt == 2) {
                minAmount = _getCurrentTotalMinRaise();
            } else {
                if (actionInt - 3 < 0 || static_cast<size_t>(actionInt - 3) >= betSizesListAsFracOfPot.size()) {
#ifdef DEBUG_POKER_ENV
                    std::cerr << "Error: actionInt-3 out of bounds for betSizesListAsFracOfPot (minAmount calc)." << std::endl;
#endif
                    minAmount = raiseAmount;
                } else {
                    float smallerFraction = betSizesListAsFracOfPot[actionInt - 3];
                    int smallerAmount = getFractionOfPotRaise(smallerFraction, players[currentPlayer]);
                    minAmount = (raiseAmount + smallerAmount) / 2;
                }
            }

            if (minAmount >= maxAmount) {
                return {static_cast<float>(BET_RAISE), static_cast<float>(minAmount)};
            }

            std::uniform_int_distribution<> dis(minAmount, maxAmount -1);
            int randomAmount = dis(m_rng);

            return {static_cast<float>(BET_RAISE), static_cast<float>(randomAmount)};
        } else {
            return {static_cast<float>(BET_RAISE), static_cast<float>(raiseAmount)};
        }
    }

#ifdef DEBUG_POKER_ENV
    std::cerr << "Invalid actionInt: " << actionInt << " in _getEnvAdjustedActionFormulation" << std::endl;
#endif
    return {static_cast<float>(CHECK_CALL), -1.0f};
}
// --- End of _getEnvAdjustedActionFormulation ---


// --- Start of _getFixedAction (new method) ---
std::vector<float> PokerEnv::_getFixedAction(const std::vector<float>& action) {
    if (action.empty()) {
        throw std::runtime_error("Empty action vector in _getFixedAction");
    }
    int actionIdx = static_cast<int>(action[0]);
    float intendedRaiseTotalAmount = action.size() > 1 ? action[1] : 0.0f;

    if (currentPlayer < 0 || currentPlayer >= N_SEATS || !players[currentPlayer]) {
        throw std::runtime_error("Current player not set or invalid in _getFixedAction");
    }
    PokerPlayer* player = players[currentPlayer];

    int totalToCall = _getBiggestBetOutThereAkaTotalToCall();

    if (actionIdx == FOLD) {
        if (totalToCall <= player->currentBet) {
            return _processCheckCall(totalToCall);
        }
        return {static_cast<float>(FOLD), -1.0f};
    } else if (actionIdx == CHECK_CALL) {
        if (FIRST_ACTION_NO_CALL && (nActionsThisEpisode == 0) && (currentRound == PREFLOP)) {
            return {static_cast<float>(FOLD), -1.0f};
        }
        return _processCheckCall(totalToCall);
    } else if (actionIdx == BET_RAISE) {
        if (IS_FIXED_LIMIT_GAME && currentRound != PREFLOP) {  // 翻前不受限制
            if (currentRound >= 0 && static_cast<size_t>(currentRound) < MAX_N_RAISES_PER_ROUND.size()) {
                if (nRaisesThisRound >= MAX_N_RAISES_PER_ROUND[currentRound]) {
                    return _processCheckCall(totalToCall);
                }
            } else {
#ifdef DEBUG_POKER_ENV
                 std::cerr << "Warning: currentRound out of bounds for MAX_N_RAISES_PER_ROUND. Defaulting to Call." << std::endl;
#endif
                 return _processCheckCall(totalToCall);
            }
        }

        if ((player->stack + player->currentBet <= totalToCall) ||
            (cappedRaise_member.happenedThisRound && cappedRaise_member.playerThatCantReopen == currentPlayer)) {
            return _processCheckCall(totalToCall);
        }

            // If the last raiser is all-in, and no other players (who are not the current player and not the last raiser)
            // are still in the hand and NOT all-in and have chips, the current player cannot re-raise. They can only call or fold.
            if (lastRaiser != -1 && players[lastRaiser]->isAllin) {
                bool other_active_players_exist = false;
                for (int i = 0; i < N_SEATS; ++i) {
                    if (i == currentPlayer || i == lastRaiser)
                        continue;
                    if (!players[i]->folded && !players[i]->isAllin && players[i]->stack > 0) {
                        other_active_players_exist = true;
                        break;
                    }
                }
                if (!other_active_players_exist) {
                    // Only the current player and the all-in lastRaiser are effectively in contention for further betting.
                    // Current player cannot re-raise.
                    return _processCheckCall(totalToCall);
                }
            }

        return _processRaise(intendedRaiseTotalAmount);
    } else {
#ifdef DEBUG_POKER_ENV
        std::cerr << "Invalid action index (" << actionIdx << "), must be FOLD (0), CHECK/CALL (1), or BET/RAISE (2)" << std::endl;
#endif
        throw std::runtime_error("Invalid action index in _getFixedAction");
    }
}
// --- End of _getFixedAction ---

void PokerEnv::_calculateRewardScalar() {
    bool scaleRewards = false;

#ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG] _calculateRewardScalar: 开始执行" << std::endl;
    std::cout << "[DEBUG] args_config内容: " << args_config.dump() << std::endl;
#endif

    // Check if scale_rewards is enabled in config
    if (args_config.contains("reward_settings") && args_config["reward_settings"].is_object() &&
        args_config["reward_settings"].contains("scale_rewards") && args_config["reward_settings"]["scale_rewards"].is_boolean()) {
        scaleRewards = args_config["reward_settings"]["scale_rewards"].get<bool>();
#ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] 从配置中读取到 scale_rewards: " << scaleRewards << std::endl;
#endif
    } else {
#ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] 配置中没有找到valid的scale_rewards设置，使用默认值false" << std::endl;
        if (!args_config.contains("reward_settings")) {
            std::cout << "[DEBUG] - 没有reward_settings部分" << std::endl;
        } else if (!args_config["reward_settings"].is_object()) {
            std::cout << "[DEBUG] - reward_settings不是对象" << std::endl;
        } else if (!args_config["reward_settings"].contains("scale_rewards")) {
            std::cout << "[DEBUG] - reward_settings中没有scale_rewards" << std::endl;
        } else if (!args_config["reward_settings"]["scale_rewards"].is_boolean()) {
            std::cout << "[DEBUG] - scale_rewards不是布尔值" << std::endl;
        }
#endif
    }

    if (scaleRewards) {
        // Calculate average stack based on total chips (stack + currentBet), like in getPublicObservation
        float averageStack = 0.0f;
        if (!players.empty()) {
            for (const auto* p : players) {
                averageStack += p->startingStack; // Use startingStack for consistency
            }
            averageStack /= static_cast<float>(players.size());
        }

        if (averageStack <= 0.0f) {
            averageStack = static_cast<float>(DEFAULT_STACK_SIZE); // Fallback
        }

        REWARD_SCALAR = averageStack;
        if (REWARD_SCALAR == 0.0f) REWARD_SCALAR = 1.0f; // Avoid division by zero

#ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] scale_rewards=true, 计算averageStack=" << averageStack
                  << ", REWARD_SCALAR=" << REWARD_SCALAR << std::endl;
#endif
    } else {
        REWARD_SCALAR = 1.0f;
#ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG] scale_rewards=false, 设置REWARD_SCALAR=1.0" << std::endl;
#endif
    }

#ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG] _calculateRewardScalar: 完成，最终REWARD_SCALAR=" << REWARD_SCALAR << std::endl;
#endif
}

// Method to expose the internal LUT to Python for testing
std::map<int64_t, std::vector<float>> PokerEnv::getInternalPrivObsLut_py() const {
    return m_privObsLut;
}

// Private helper to parse board card string
void PokerEnv::_parseBoardString(const std::string& boardCardsStr, std::vector<Card>& out_board_storage) const {
    out_board_storage.clear(); // Ensure it starts empty

    if (boardCardsStr.empty()) {
        return; // 0 community cards
    }

    std::vector<std::string> potential_card_strs;
    std::string current_segment;
    std::istringstream iss(boardCardsStr);

    bool has_spaces = (boardCardsStr.find(' ') != std::string::npos);

    if (has_spaces) {
        while (iss >> current_segment) {
            potential_card_strs.push_back(current_segment);
        }

        if (potential_card_strs.empty() && !boardCardsStr.empty()){ // e.g. boardCardsStr was just spaces
             // Proceed to no-space parsing
        } else if (potential_card_strs.size() == 0 || potential_card_strs.size() == 3 || potential_card_strs.size() == 4 || potential_card_strs.size() == 5) {
            bool all_parsed_successfully = true;
            std::vector<Card> temp_parsed_cards_for_space_branch; // Use a distinct name for this temporary vector
            for (const std::string& s : potential_card_strs) {
                try {
                    auto parsed_card = parseCardStringInternal(s);
                    temp_parsed_cards_for_space_branch.emplace_back(parsed_card.second, parsed_card.first);
                } catch (const std::invalid_argument& e) {
                    all_parsed_successfully = false;
                    #ifdef DEBUG_POKER_ENV
                    std::cerr << "[DEBUG _parseBoardString] Error parsing space-separated card string '" << s << "': " << e.what() << std::endl;
                    #endif
                    break;
                }
            }
            if (all_parsed_successfully) {
                out_board_storage = temp_parsed_cards_for_space_branch; // Assign if all were successful
                return; // Successfully parsed space-separated cards
            }
        }
        // If parsing space-separated failed or wrong number of cards, ensure out_board_storage is clear before trying concatenated parsing
        out_board_storage.clear();
    }

    // Attempt concatenated parsing if no spaces or space parsing failed
    std::string s_to_parse = boardCardsStr;
    // Remove all spaces for concatenated parsing attempt (only if has_spaces was true and previous attempt failed, or if no spaces initially)
    if (has_spaces) { // if space parsing failed, s_to_parse still contains original boardCardsStr
        s_to_parse.erase(std::remove_if(s_to_parse.begin(), s_to_parse.end(), ::isspace), s_to_parse.end());
    } else { // if no spaces to begin with, s_to_parse is already correct (boardCardsStr)
        // No action needed on s_to_parse if there were no spaces originally
    }

    if (s_to_parse.empty()) { // If original was just spaces and became empty, or was empty initially
        out_board_storage.clear(); // Ensure it's empty
        return; // Treat as 0 cards
    }

    out_board_storage.clear(); // Clear for this concatenated parsing attempt
    size_t current_pos = 0;
    while(current_pos < s_to_parse.length() && out_board_storage.size() < 5) { // check out_board_storage.size()
        std::string best_card_str_found;
        std::pair<Card::CardValue, Card::Suit> parsed_card_data;
        bool current_card_successfully_parsed = false;

        for (size_t len_try = std::min((size_t)5, s_to_parse.length() - current_pos); len_try >= 2; --len_try) {
            std::string potential_card = s_to_parse.substr(current_pos, len_try);
            try {
                parsed_card_data = parseCardStringInternal(potential_card);
                best_card_str_found = potential_card;
                current_card_successfully_parsed = true;
                break;
            } catch (const std::invalid_argument& e) {
                // Not a valid card of this length, try shorter.
            }
        }

        if (current_card_successfully_parsed) {
            out_board_storage.emplace_back(parsed_card_data.second, parsed_card_data.first);
            current_pos += best_card_str_found.length();
        } else {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR _parseBoardString] Failed to parse card from remaining non-space string: '" << s_to_parse.substr(current_pos) << "' in full string '" << boardCardsStr << "'" << std::endl;
            #endif
            out_board_storage.clear(); // Parsing failed, invalid board
            return;
        }
    }

    size_t num_parsed_cards = out_board_storage.size();
    if (num_parsed_cards != 0 && num_parsed_cards != 3 && num_parsed_cards != 4 && num_parsed_cards != 5) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR _parseBoardString] Invalid number of board cards parsed (" << num_parsed_cards << ") from: '" << boardCardsStr << "'" << std::endl;
        #endif
        out_board_storage.clear(); // Invalid number of cards
    }
}


int PokerEnv::getHandRank(int64_t rangeIdx, const std::string& boardCardsStr) const {
    auto it_idx_to_range = m_idxToRangeLut.find(rangeIdx);
    if (it_idx_to_range == m_idxToRangeLut.end()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(rangeIdx, string_board)] Invalid rangeIdx provided: " << rangeIdx << std::endl;
        #endif
        return 0; // Invalid rangeIdx
    }

    // m_idxToRangeLut stores pairs of 1D card indices (0-51)
    // We need to convert these to Card::CardValue and Card::Suit
    int card1_1d = it_idx_to_range->second.first;
    int card2_1d = it_idx_to_range->second.second;

    // Convert 1D indices to CardValue and Suit
    // Card layout: 0-12 = 2♢-A♢, 13-25 = 2♣-A♣, 26-38 = 2♡-A♡, 39-51 = 2♠-A♠
    Card::Suit card1_suit = static_cast<Card::Suit>(card1_1d / 13);
    Card::CardValue card1_value = static_cast<Card::CardValue>(card1_1d % 13);
    Card::Suit card2_suit = static_cast<Card::Suit>(card2_1d / 13);
    Card::CardValue card2_value = static_cast<Card::CardValue>(card2_1d % 13);

    std::vector<Card> local_hole_cards_storage;
    local_hole_cards_storage.reserve(2);

    // Create Card objects with proper suit and value
    local_hole_cards_storage.emplace_back(card1_suit, card1_value);
    local_hole_cards_storage.emplace_back(card2_suit, card2_value);

    std::vector<Card*> hand_card_ptrs; // Pointers to the locally stored hole cards
    if (local_hole_cards_storage.size() == 2) {
        hand_card_ptrs.push_back(&local_hole_cards_storage[0]);
        hand_card_ptrs.push_back(&local_hole_cards_storage[1]);
    } else {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(rangeIdx, string_board)] Failed to construct local hole cards. rangeIdx: " << rangeIdx << std::endl;
        #endif
        return 0; // Error case
    }

    std::vector<Card> local_board_storage_for_this_call; // Stores actual Card objects for board cards
    _parseBoardString(boardCardsStr, local_board_storage_for_this_call);

    std::vector<Card*> board_card_ptrs; // Pointers to the locally stored board cards
    board_card_ptrs.reserve(local_board_storage_for_this_call.size());
    for (size_t i = 0; i < local_board_storage_for_this_call.size(); ++i) {
        board_card_ptrs.push_back(&local_board_storage_for_this_call[i]);
    }

    std::vector<Card*> all_cards_for_eval = hand_card_ptrs;
    all_cards_for_eval.insert(all_cards_for_eval.end(), board_card_ptrs.begin(), board_card_ptrs.end());

    // Convert to phevaluator's int representation
    std::vector<int> eval_cards_int;
    eval_cards_int.reserve(all_cards_for_eval.size());
    for(Card* c_ptr : all_cards_for_eval) {
        if (c_ptr) { // Ensure card pointer is valid
            eval_cards_int.push_back(_convert_card_to_phevaluator_int(c_ptr));
        }
    }
    // Filter out any -1 (error indicators from conversion)
    eval_cards_int.erase(std::remove(eval_cards_int.begin(), eval_cards_int.end(), -1), eval_cards_int.end());

    if (eval_cards_int.size() < 5) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG PokerEnv::getHandRank(rangeIdx, string_board)] Need at least 5 cards for evaluation, got "
                  << eval_cards_int.size() << ". Hole cards from rangeIdx: " << rangeIdx
                  << " (vals " << static_cast<int>(local_hole_cards_storage[0].getValue()) << Card::SuitString[local_hole_cards_storage[0].getSuit()]
                  << " " << static_cast<int>(local_hole_cards_storage[1].getValue()) << Card::SuitString[local_hole_cards_storage[1].getSuit()]
                  << "), Board string: '" << boardCardsStr << "'. Returning 0 (unrankable)." << std::endl;
        #endif
        return 0; // Not enough cards to evaluate
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRank(rangeIdx, string_board)] rangeIdx: " << rangeIdx
              << ", Hole: " << local_hole_cards_storage[0].toString() << " " << local_hole_cards_storage[1].toString()
              << ", Board: " << boardCardsStr << std::endl;
    std::cout << "  eval_cards_int for phevaluator (size " << eval_cards_int.size() << "): ";
    for (size_t i = 0; i < eval_cards_int.size(); ++i) {
        std::cout << eval_cards_int[i] << (i == eval_cards_int.size() - 1 ? "" : ", ");
    }
    std::cout << std::endl;
    std::cout << "  eval_cards_int breakdown (pheval_rank, pheval_suit): ";
    for (int card_int : eval_cards_int) {
        if (card_int != -1) {
             std::cout << "(" << (card_int / 4) << "," << (card_int % 4) << ") ";
        } else {
             std::cout << "(-1) ";
        }
    }
    std::cout << std::endl;
    #endif

    // Call the appropriate phevaluator function based on the number of cards
    int rank_val = 7463; // Default to phevaluator's worst rank representation + 1
    if (eval_cards_int.size() == 5) {
        rank_val = evaluate_5cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4]);
    } else if (eval_cards_int.size() == 6) {
        rank_val = evaluate_6cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5]);
    } else if (eval_cards_int.size() == 7) { // Max 7 cards
        rank_val = evaluate_7cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5], eval_cards_int[6]);
    } else if (eval_cards_int.size() > 7) {
         #ifdef DEBUG_POKER_ENV
        std::cout << "[WARNING PokerEnv::getHandRank(vec_card*, vec_card*)] More than 7 cards after processing (" << eval_cards_int.size() << "). Evaluating first 7." << std::endl;
        #endif
        rank_val = evaluate_7cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5], eval_cards_int[6]);
    }
    // phevaluator: 1 is best (Royal/Straight Flush), 7462 is worst (High Card).
    // Return phevaluator result directly: 1 = best, 7462 = worst
    // Ensure 0 from pheval (error/no rank) remains 0. (pheval returns 0 for error if input is bad, e.g. duplicate cards)
    return rank_val == 0 ? 0 : rank_val;
}

int PokerEnv::getHandRank(const std::string& twoHoleCardsStr, const std::string& boardCardsStr) const {
    if (twoHoleCardsStr.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Provided empty string for hole cards."<< std::endl;
        #endif
        return 0; // Error/worst rank
    }

    std::string cardStr1_local, cardStr2_local;
    // Logic to parse twoHoleCardsStr (copied and adapted from getRangeIdxByHand)
    size_t space_pos = twoHoleCardsStr.find(' ');
    if (space_pos != std::string::npos) {
        cardStr1_local = twoHoleCardsStr.substr(0, space_pos);
        size_t first_char_card2 = twoHoleCardsStr.find_first_not_of(' ', space_pos);
        if (first_char_card2 != std::string::npos) {
            cardStr2_local = twoHoleCardsStr.substr(first_char_card2);
        } else {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Malformed hole card string with space: '" << twoHoleCardsStr << "'" << std::endl;
            #endif
            return 0;
        }
    } else { // No spaces, could be "AsKs" or "10sKs" or unicode "A♠K♠"
        bool found_split = false;
        // Try splitting by finding the start of a second valid card string.
        // Iterate potential lengths for the first card (2 to 5 chars typical, e.g. "As", "10s", "A♠", "10♠")
        for (size_t len_card1 = 2; len_card1 <= 5 && len_card1 < twoHoleCardsStr.length(); ++len_card1) {
            std::string potential_card1_str = twoHoleCardsStr.substr(0, len_card1);
            std::string potential_card2_str = twoHoleCardsStr.substr(len_card1);

            if (potential_card2_str.length() < 2 || potential_card2_str.length() > 5) continue; // Basic length check for second card

            // Attempt to parse both to validate the split
            try {
                parseCardStringInternal(potential_card1_str); // Validate part1
                parseCardStringInternal(potential_card2_str); // Validate part2
                cardStr1_local = potential_card1_str;
                cardStr2_local = potential_card2_str;
                found_split = true;
                break;
            } catch (const std::invalid_argument&) { /* This split is invalid, try next */ }
        }
        if (!found_split) {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Failed to split no-space hole card string: '" << twoHoleCardsStr << "'" << std::endl;
            #endif
            return 0;
        }
    }

    if (cardStr1_local.empty() || cardStr2_local.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] One or both card strings empty after hole split attempt for: '" << twoHoleCardsStr << "'" << std::endl;
        #endif
        return 0;
    }

    std::vector<Card> local_hole_cards_storage; // Stores actual Card objects for hole cards
    std::vector<Card*> hand_card_ptrs;        // Pointers to the locally stored hole cards
    try {
        auto parsedCard1_data = parseCardStringInternal(cardStr1_local); // Returns pair: <Value, Suit>
        auto parsedCard2_data = parseCardStringInternal(cardStr2_local); // Returns pair: <Value, Suit>

        // Card constructor is Card(CardValue val, CardSuit suit) OR Card(CardSuit suit, CardValue val)
        // parseCardStringInternal returns std::pair<Card::CardValue, Card::Suit>
        // So it should be Card(parsedCard_data.first, parsedCard_data.second)
        local_hole_cards_storage.emplace_back(parsedCard1_data.second, parsedCard1_data.first);
        local_hole_cards_storage.emplace_back(parsedCard2_data.second, parsedCard2_data.first);

        if (local_hole_cards_storage.size() == 2) {
            hand_card_ptrs.push_back(&local_hole_cards_storage[0]);
            hand_card_ptrs.push_back(&local_hole_cards_storage[1]);
        } else {
             #ifdef DEBUG_POKER_ENV
             std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Failed to construct local hole cards from strings. Card1: '"
                       << cardStr1_local << "', Card2: '" << cardStr2_local << "'" << std::endl;
             #endif
             return 0; // Error case
        }
    } catch (const std::invalid_argument& e) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Exception parsing hole card strings. Card1: '"
                  << cardStr1_local << "', Card2: '" << cardStr2_local << "'. Error: " << e.what() << std::endl;
        #endif
        return 0; // Error/worst rank
    }

    std::vector<Card> local_board_storage_for_this_call; // Stores actual Card objects for board cards
    _parseBoardString(boardCardsStr, local_board_storage_for_this_call);

    std::vector<Card*> board_card_ptrs;   // Pointers to the locally stored board cards
    board_card_ptrs.reserve(local_board_storage_for_this_call.size());
    for (size_t i = 0; i < local_board_storage_for_this_call.size(); ++i) {
        board_card_ptrs.push_back(&local_board_storage_for_this_call[i]);
    }

    std::vector<Card*> all_cards_for_eval = hand_card_ptrs;
    all_cards_for_eval.insert(all_cards_for_eval.end(), board_card_ptrs.begin(), board_card_ptrs.end());

    // Convert to phevaluator's int representation
    std::vector<int> eval_cards_int;
    eval_cards_int.reserve(all_cards_for_eval.size());
    for(Card* c_ptr : all_cards_for_eval) {
        if (c_ptr) { // Ensure card pointer is valid
            eval_cards_int.push_back(_convert_card_to_phevaluator_int(c_ptr));
        }
    }
    // Filter out any -1 (error indicators from conversion)
    eval_cards_int.erase(std::remove(eval_cards_int.begin(), eval_cards_int.end(), -1), eval_cards_int.end());

    if (eval_cards_int.size() < 5) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG PokerEnv::getHandRank(string_hole, string_board)] Need at least 5 cards for evaluation, got "
                  << eval_cards_int.size() << ". Hole cards: '" << twoHoleCardsStr << "', Board string: '" << boardCardsStr << "'. Returning 0 (unrankable)." << std::endl;
        #endif
        return 0; // Not enough cards
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRank(string_hole, string_board)] Hole: " << twoHoleCardsStr << ", Board: " << boardCardsStr << std::endl;
    std::cout << "  eval_cards_int for phevaluator (size " << eval_cards_int.size() << "): ";
    for (size_t i = 0; i < eval_cards_int.size(); ++i) {
        std::cout << eval_cards_int[i] << (i == eval_cards_int.size() - 1 ? "" : ", ");
    }
    std::cout << std::endl;
    std::cout << "  eval_cards_int breakdown (pheval_rank, pheval_suit): ";
     for (int card_int : eval_cards_int) {
        if (card_int != -1) {
             std::cout << "(" << (card_int / 4) << "," << (card_int % 4) << ") ";
        } else {
             std::cout << "(-1) ";
        }
    }
    std::cout << std::endl;
    #endif

    int rank_val = 7463; // Default to phevaluator's worst rank representation + 1
    if (eval_cards_int.size() == 5) {
        rank_val = evaluate_5cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4]);
    } else if (eval_cards_int.size() == 6) {
        rank_val = evaluate_6cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5]);
    } else if (eval_cards_int.size() == 7) { // Max 7 cards
        rank_val = evaluate_7cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5], eval_cards_int[6]);
    } else if (eval_cards_int.size() > 7) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[WARNING PokerEnv::getHandRank(string_hole, string_board)] More than 7 cards after parsing (" << eval_cards_int.size() << "). Evaluating first 7." << std::endl;
        #endif
        rank_val = evaluate_7cards(eval_cards_int[0], eval_cards_int[1], eval_cards_int[2], eval_cards_int[3], eval_cards_int[4], eval_cards_int[5], eval_cards_int[6]);
    }
    // Return phevaluator result directly: 1 = best, 7462 = worst
    return rank_val == 0 ? 0 : rank_val;
}

int PokerEnv::getHandRank(int64_t rangeIdx, const std::vector<Card*>& board_cards) const {
    auto it_idx_to_range = m_idxToRangeLut.find(rangeIdx);
    if (it_idx_to_range == m_idxToRangeLut.end()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(rangeIdx, vec_board*)] Invalid rangeIdx: " << rangeIdx << std::endl;
        #endif
        return 0; // Invalid rangeIdx
    }

    // m_idxToRangeLut stores pairs of 1D card indices (0-51)
    // We need to convert these to Card::CardValue and Card::Suit
    int card1_1d = it_idx_to_range->second.first;
    int card2_1d = it_idx_to_range->second.second;

    // Convert 1D indices to CardValue and Suit
    // Card layout: 0-12 = 2♢-A♢, 13-25 = 2♣-A♣, 26-38 = 2♡-A♡, 39-51 = 2♠-A♠
    Card::Suit card1_suit = static_cast<Card::Suit>(card1_1d / 13);
    Card::CardValue card1_value = static_cast<Card::CardValue>(card1_1d % 13);
    Card::Suit card2_suit = static_cast<Card::Suit>(card2_1d / 13);
    Card::CardValue card2_value = static_cast<Card::CardValue>(card2_1d % 13);

    // Store Card objects locally to ensure their lifetime
    std::vector<Card> local_hole_cards_storage;
    local_hole_cards_storage.reserve(2);
    local_hole_cards_storage.emplace_back(card1_suit, card1_value);
    local_hole_cards_storage.emplace_back(card2_suit, card2_value);

    std::vector<Card*> temp_hole_cards_ptrs = {&local_hole_cards_storage[0], &local_hole_cards_storage[1]};

    // Now call the main getHandRank with the constructed hole card pointers and given board_cards
    return getHandRank(temp_hole_cards_ptrs, board_cards);
}

int PokerEnv::getHandRank(const std::string& twoHoleCardsStr, const std::vector<Card*>& board_cards) const {
    if (twoHoleCardsStr.empty()) {
        return 0; // Worst rank
    }

    std::string cardStr1_local, cardStr2_local;
    // Logic to parse twoHoleCardsStr into cardStr1_local and cardStr2_local
    // (Copied and adapted from existing getRangeIdxByHand(const std::string& twoCardsStr))
    size_t space_pos = twoHoleCardsStr.find(' ');
    if (space_pos != std::string::npos) {
        cardStr1_local = twoHoleCardsStr.substr(0, space_pos);
        size_t first_char_card2 = twoHoleCardsStr.find_first_not_of(' ', space_pos);
        if (first_char_card2 != std::string::npos) {
            cardStr2_local = twoHoleCardsStr.substr(first_char_card2);
        } else {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR getHandRank(string, vec_card*)] Malformed input with space: " << twoHoleCardsStr << std::endl;
            #endif
            return 0;
        }
    } else {
        bool found_split = false;
        if (twoHoleCardsStr.length() >= 4 && twoHoleCardsStr.length() <= 10) {
            for (size_t len_card1 = 2; len_card1 <= 5 && len_card1 < twoHoleCardsStr.length(); ++len_card1) {
                std::string potential_card1_str = twoHoleCardsStr.substr(0, len_card1);
                std::string potential_card2_str = twoHoleCardsStr.substr(len_card1);

                if (potential_card2_str.length() < 2 || potential_card2_str.length() > 5) { // Sanity check for card 2 length
                    continue;
                }

                try {
                    parseCardStringInternal(potential_card1_str); // Throws on error
                    parseCardStringInternal(potential_card2_str); // Throws on error
                    // If both parsed successfully, this is our split
                    cardStr1_local = potential_card1_str;
                    cardStr2_local = potential_card2_str;
                    found_split = true;
                    break;
                } catch (const std::invalid_argument& e) {
                    // This split was invalid, try next length for card1
                    #ifdef DEBUG_POKER_ENV
                    // std::cout << "[DEBUG getRangeIdxByHand single_str] Split attempt failed for "
                    //           << potential_card1_str << " / " << potential_card2_str << ": " << e.what() << std::endl;
                    #endif
                }
            }
        }

        if (!found_split) {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR getHandRank(string, vec_card*)] Could not determine valid split for no-space string: " << twoHoleCardsStr << std::endl;
            #endif
            return 0; // Corrected: Could not split, return 0 for worst rank/error
        }
    }

    // The #ifdef DEBUG_POKER_ENV std::cout line that was here previously will be removed by this edit.
    // Corrected logic for handling parsed card strings and calling base getHandRank:
    if (cardStr1_local.empty() || cardStr2_local.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandRank(string_hole, vec_board*)] Empty card string after parsing twoHoleCardsStr: '" << twoHoleCardsStr << "'" << std::endl;
        #endif
        return 0; // Return 0 for error/worst rank
    }

    try {
        auto parsedCard1 = parseCardStringInternal(cardStr1_local);
        auto parsedCard2 = parseCardStringInternal(cardStr2_local);

        std::vector<Card> local_hole_cards_storage; // Stores actual Card objects for hole cards
        local_hole_cards_storage.emplace_back(parsedCard1.second, parsedCard1.first);
        local_hole_cards_storage.emplace_back(parsedCard2.second, parsedCard2.first);

        std::vector<Card*> temp_hole_cards_ptrs; // Pointers to the locally stored hole cards
        if (local_hole_cards_storage.size() == 2) { // Ensure cards were actually added
            temp_hole_cards_ptrs.push_back(&local_hole_cards_storage[0]);
            temp_hole_cards_ptrs.push_back(&local_hole_cards_storage[1]);
        } else {
            // This case should ideally not be reached if parsing logic for twoHoleCardsStr is robust
            // and cardStr1_local/cardStr2_local are guaranteed non-empty and valid if we reach here.
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR getHandRank(string_hole, vec_board*)] Failed to create local Card objects from twoHoleCardsStr: '" << twoHoleCardsStr << "'. Parsed strings: '" << cardStr1_local << "', '" << cardStr2_local << "'" << std::endl;
            #endif
            return 0; // Error case
        }

        // Call the base getHandRank that takes two std::vector<Card*>
        // The board_cards are passed through as they are already Card* (lifetime managed by caller)
        return getHandRank(temp_hole_cards_ptrs, board_cards);
    } catch (const std::invalid_argument& e) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandRank(string_hole, vec_board*)] Exception parsing card strings ('" << cardStr1_local << "', '" << cardStr2_local << "') from twoHoleCardsStr ('" << twoHoleCardsStr << "'): " << e.what() << std::endl;
        #endif
        return 0; // Worst rank on parsing error
    }
}

int PokerEnv::_convert_card_to_phevaluator_int(const Card* card) const {
    if (!card) {
        // Handle null card pointer, though in context this should ideally not happen
        // if called with valid cards. Returning a value that would be out of phevaluator's typical range
        // or a specific error code might be an option, but pheval expects valid card ints.
        // For now, let's throw or return an obviously problematic value if this occurs.
        // However, the callers (getHandRank) already filter out nullptrs usually.
        // Let's assume card is valid based on typical usage context.
        // If rigorous error handling is needed here, an exception or specific return might be better.
        // For simplicity, mirroring previous direct conversion approach:
        // Though this should not happen in practice with proper calling
        return -1; // Indicates error
    }

    // Get our internal representations
    int internal_rank = static_cast<int>(card->getValue()); // 0-12: Two=0, Three=1, ..., Ace=12
    int internal_suit = static_cast<int>(card->getSuit());  // 0-3: Diamonds=0, Hearts=1, Clubs=2, Spades=3

    // phevaluator expects: rank * 4 + suit
    // where rank: deuce=0, trey=1, ..., ace=12 (matches our CardValue)
    // where suit: club=0, diamond=1, heart=2, spade=3

    // Calculate phevaluator card ID: rank * 4 + suit
    return internal_rank * 4 + internal_suit;
}

// 新增：牌力比较和排序相关方法
int PokerEnv::compareHandStrength(const PokerPlayer* player1, const PokerPlayer* player2) const {
    if (!player1 || !player2) {
        return 0;
    }

    // 获取两个玩家的牌力等级（phevaluator中数值越小牌力越强）
    int rank1 = getHandRank(player1->hand, communityCards);
    int rank2 = getHandRank(player2->hand, communityCards);

    // 返回比较结果：
    // -1: player1 更强
    //  0: 相等
    //  1: player2 更强
    if (rank1 < rank2) return -1;
    if (rank1 > rank2) return 1;
    return 0;
}

std::vector<PokerPlayer*> PokerEnv::sortPlayersByHandStrength(const std::vector<PokerPlayer*>& players, bool ascending) const {
    std::vector<PokerPlayer*> sortedPlayers = players;

    // 创建一个helper对象来避免lambda中的const问题
    auto compareHelper = [this](const PokerPlayer* a, const PokerPlayer* b, bool asc) -> bool {
        if (!a || !b) return false;

        int rankA = this->getHandRank(a->hand, this->communityCards);
        int rankB = this->getHandRank(b->hand, this->communityCards);

        // phevaluator中数值越小牌力越强
        if (asc) {
            return rankA < rankB; // 从强到弱
        } else {
            return rankA > rankB; // 从弱到强
        }
    };

    std::sort(sortedPlayers.begin(), sortedPlayers.end(),
              [this, ascending, compareHelper](const PokerPlayer* a, const PokerPlayer* b) {
                  return compareHelper(a, b, ascending);
              });

    return sortedPlayers;
}

std::vector<std::vector<PokerPlayer*>> PokerEnv::groupPlayersByHandStrength(const std::vector<PokerPlayer*>& players) const {
    std::vector<std::vector<PokerPlayer*>> groups;

    if (players.empty()) {
        return groups;
    }

    // 首先按牌力排序（从强到弱）
    std::vector<PokerPlayer*> sortedPlayers = sortPlayersByHandStrength(players, true);

    // 将相同牌力的玩家分组
    std::vector<PokerPlayer*> currentGroup;
    int currentRank = -1;

    for (PokerPlayer* player : sortedPlayers) {
        if (!player) continue;

        int playerRank = getHandRank(player->hand, communityCards);

        if (currentRank == -1) {
            // 第一个玩家
            currentRank = playerRank;
            currentGroup.push_back(player);
        } else if (playerRank == currentRank) {
            // 相同牌力，加入当前组
            currentGroup.push_back(player);
        } else {
            // 不同牌力，保存当前组并开始新组
            if (!currentGroup.empty()) {
                groups.push_back(currentGroup);
            }
            currentGroup.clear();
            currentGroup.push_back(player);
            currentRank = playerRank;
        }
    }

    // 添加最后一组
    if (!currentGroup.empty()) {
        groups.push_back(currentGroup);
    }

    return groups;
}

// Add new method after existing getHandRank methods
int PokerEnv::getHandRankWithPotential(int64_t rangeIdx, const std::string& boardCardsStr) const {
    auto it_idx_to_range = m_idxToRangeLut.find(rangeIdx);
    if (it_idx_to_range == m_idxToRangeLut.end()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRankWithPotential] Invalid rangeIdx provided: " << rangeIdx << std::endl;
        #endif
        return 0; // Invalid rangeIdx
    }

    // Get hole cards from range index
    int card1_1d = it_idx_to_range->second.first;
    int card2_1d = it_idx_to_range->second.second;

    // Convert to Card objects
    Card::Suit card1_suit = static_cast<Card::Suit>(card1_1d / 13);
    Card::CardValue card1_value = static_cast<Card::CardValue>(card1_1d % 13);
    Card::Suit card2_suit = static_cast<Card::Suit>(card2_1d / 13);
    Card::CardValue card2_value = static_cast<Card::CardValue>(card2_1d % 13);

    std::vector<Card> local_hole_cards_storage;
    local_hole_cards_storage.emplace_back(card1_suit, card1_value);
    local_hole_cards_storage.emplace_back(card2_suit, card2_value);

    // Parse board cards
    std::vector<Card> local_board_storage;
    _parseBoardString(boardCardsStr, local_board_storage);

    // Convert cards to phevaluator format
    int h1 = _convert_card_to_phevaluator_int(&local_hole_cards_storage[0]);
    int h2 = _convert_card_to_phevaluator_int(&local_hole_cards_storage[1]);

    int c1 = -1, c2 = -1, c3 = -1, c4 = -1, c5 = -1;
    if (local_board_storage.size() > 0) c1 = _convert_card_to_phevaluator_int(&local_board_storage[0]);
    if (local_board_storage.size() > 1) c2 = _convert_card_to_phevaluator_int(&local_board_storage[1]);
    if (local_board_storage.size() > 2) c3 = _convert_card_to_phevaluator_int(&local_board_storage[2]);
    if (local_board_storage.size() > 3) c4 = _convert_card_to_phevaluator_int(&local_board_storage[3]);
    if (local_board_storage.size() > 4) c5 = _convert_card_to_phevaluator_int(&local_board_storage[4]);

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRankWithPotential] rangeIdx: " << rangeIdx
              << ", Hole: " << local_hole_cards_storage[0].toString() << " " << local_hole_cards_storage[1].toString()
              << ", Board: " << boardCardsStr << ", Auto-detected stage based on board size: " << local_board_storage.size() << std::endl;
    #endif

    // Collect all cards into array for new API
    std::vector<int> all_cards;
    all_cards.push_back(h1);
    all_cards.push_back(h2);
    if (c1 != -1) all_cards.push_back(c1);
    if (c2 != -1) all_cards.push_back(c2);
    if (c3 != -1) all_cards.push_back(c3);
    if (c4 != -1) all_cards.push_back(c4);
    if (c5 != -1) all_cards.push_back(c5);

    // Call our new potential evaluator (stage auto-determined)
    int potential_rank = evaluate_holdem_with_potential(all_cards.data(), all_cards.size());

    // To maintain backward compatibility with a system expecting rank (lower is better),
    // we convert strength back to a pseudo-rank.
    // A simple inversion is sufficient: Rank = MaxStrength - CurrentStrength
    return 1000000 - potential_rank;
}

int PokerEnv::getHandRankWithPotential(const std::string& twoHoleCardsStr, const std::string& boardCardsStr) const {
    // 暂时使用标准评估，后续完善潜力评估
        if (twoHoleCardsStr.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Provided empty string for hole cards."<< std::endl;
        #endif
        return 0; // Error/worst rank
    }

    std::string cardStr1_local, cardStr2_local;
    // Logic to parse twoHoleCardsStr (copied and adapted from getRangeIdxByHand)
    size_t space_pos = twoHoleCardsStr.find(' ');
    if (space_pos != std::string::npos) {
        cardStr1_local = twoHoleCardsStr.substr(0, space_pos);
        size_t first_char_card2 = twoHoleCardsStr.find_first_not_of(' ', space_pos);
        if (first_char_card2 != std::string::npos) {
            cardStr2_local = twoHoleCardsStr.substr(first_char_card2);
        } else {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Malformed hole card string with space: '" << twoHoleCardsStr << "'" << std::endl;
            #endif
            return 0;
        }
    } else { // No spaces, could be "AsKs" or "10sKs" or unicode "A♠K♠"
        bool found_split = false;
        // Try splitting by finding the start of a second valid card string.
        // Iterate potential lengths for the first card (2 to 5 chars typical, e.g. "As", "10s", "A♠", "10♠")
        for (size_t len_card1 = 2; len_card1 <= 5 && len_card1 < twoHoleCardsStr.length(); ++len_card1) {
            std::string potential_card1_str = twoHoleCardsStr.substr(0, len_card1);
            std::string potential_card2_str = twoHoleCardsStr.substr(len_card1);

            if (potential_card2_str.length() < 2 || potential_card2_str.length() > 5) continue; // Basic length check for second card

            // Attempt to parse both to validate the split
            try {
                parseCardStringInternal(potential_card1_str); // Validate part1
                parseCardStringInternal(potential_card2_str); // Validate part2
                cardStr1_local = potential_card1_str;
                cardStr2_local = potential_card2_str;
                found_split = true;
                break;
            } catch (const std::invalid_argument&) { /* This split is invalid, try next */ }
        }
        if (!found_split) {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Failed to split no-space hole card string: '" << twoHoleCardsStr << "'" << std::endl;
            #endif
            return 0;
        }
    }

    if (cardStr1_local.empty() || cardStr2_local.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] One or both card strings empty after hole split attempt for: '" << twoHoleCardsStr << "'" << std::endl;
        #endif
        return 0;
    }

    std::vector<Card> local_hole_cards_storage; // Stores actual Card objects for hole cards
    std::vector<Card*> hand_card_ptrs;        // Pointers to the locally stored hole cards
    try {
        auto parsedCard1_data = parseCardStringInternal(cardStr1_local); // Returns pair: <Value, Suit>
        auto parsedCard2_data = parseCardStringInternal(cardStr2_local); // Returns pair: <Value, Suit>

        // Card constructor is Card(CardValue val, CardSuit suit) OR Card(CardSuit suit, CardValue val)
        // parseCardStringInternal returns std::pair<Card::CardValue, Card::Suit>
        // So it should be Card(parsedCard_data.first, parsedCard_data.second)
        local_hole_cards_storage.emplace_back(parsedCard1_data.second, parsedCard1_data.first);
        local_hole_cards_storage.emplace_back(parsedCard2_data.second, parsedCard2_data.first);

        if (local_hole_cards_storage.size() == 2) {
            hand_card_ptrs.push_back(&local_hole_cards_storage[0]);
            hand_card_ptrs.push_back(&local_hole_cards_storage[1]);
        } else {
             #ifdef DEBUG_POKER_ENV
             std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Failed to construct local hole cards from strings. Card1: '"
                       << cardStr1_local << "', Card2: '" << cardStr2_local << "'" << std::endl;
             #endif
             return 0; // Error case
        }
    } catch (const std::invalid_argument& e) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRank(string_hole, string_board)] Exception parsing hole card strings. Card1: '"
                  << cardStr1_local << "', Card2: '" << cardStr2_local << "'. Error: " << e.what() << std::endl;
        #endif
        return 0; // Error/worst rank
    }

    std::vector<Card> local_board_storage_for_this_call; // Stores actual Card objects for board cards
    _parseBoardString(boardCardsStr, local_board_storage_for_this_call);

    std::vector<Card*> board_card_ptrs;   // Pointers to the locally stored board cards
    board_card_ptrs.reserve(local_board_storage_for_this_call.size());
    for (size_t i = 0; i < local_board_storage_for_this_call.size(); ++i) {
        board_card_ptrs.push_back(&local_board_storage_for_this_call[i]);
    }

    std::vector<Card*> all_cards_for_eval = hand_card_ptrs;
    all_cards_for_eval.insert(all_cards_for_eval.end(), board_card_ptrs.begin(), board_card_ptrs.end());

    // Convert to phevaluator's int representation
    std::vector<int> eval_cards_int;
    eval_cards_int.reserve(all_cards_for_eval.size());
    for(Card* c_ptr : all_cards_for_eval) {
        if (c_ptr) { // Ensure card pointer is valid
            eval_cards_int.push_back(_convert_card_to_phevaluator_int(c_ptr));
        }
    }
    // Filter out any -1 (error indicators from conversion)
    eval_cards_int.erase(std::remove(eval_cards_int.begin(), eval_cards_int.end(), -1), eval_cards_int.end());

    if (eval_cards_int.size() < 5) {
        #ifdef DEBUG_POKER_ENV
        std::cout << "[DEBUG PokerEnv::getHandRank(string_hole, string_board)] Need at least 5 cards for evaluation, got "
                  << eval_cards_int.size() << ". Hole cards: '" << twoHoleCardsStr << "', Board string: '" << boardCardsStr << "'. Returning 0 (unrankable)." << std::endl;
        #endif
        return 0; // Not enough cards
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRank(string_hole, string_board)] Hole: " << twoHoleCardsStr << ", Board: " << boardCardsStr << std::endl;
    std::cout << "  eval_cards_int for phevaluator (size " << eval_cards_int.size() << "): ";
    for (size_t i = 0; i < eval_cards_int.size(); ++i) {
        std::cout << eval_cards_int[i] << (i == eval_cards_int.size() - 1 ? "" : ", ");
    }
    std::cout << std::endl;
    std::cout << "  eval_cards_int breakdown (pheval_rank, pheval_suit): ";
     for (int card_int : eval_cards_int) {
        if (card_int != -1) {
             std::cout << "(" << (card_int / 4) << "," << (card_int % 4) << ") ";
        } else {
             std::cout << "(-1) ";
        }
    }
    std::cout << std::endl;
    #endif

    // Convert hole cards to phevaluator int format
    int h1 = _convert_card_to_phevaluator_int(&local_hole_cards_storage[0]);
    int h2 = _convert_card_to_phevaluator_int(&local_hole_cards_storage[1]);

    // Convert board cards to phevaluator int format
    int c1 = -1, c2 = -1, c3 = -1, c4 = -1, c5 = -1;
    if (local_board_storage_for_this_call.size() > 0) c1 = _convert_card_to_phevaluator_int(&local_board_storage_for_this_call[0]);
    if (local_board_storage_for_this_call.size() > 1) c2 = _convert_card_to_phevaluator_int(&local_board_storage_for_this_call[1]);
    if (local_board_storage_for_this_call.size() > 2) c3 = _convert_card_to_phevaluator_int(&local_board_storage_for_this_call[2]);
    if (local_board_storage_for_this_call.size() > 3) c4 = _convert_card_to_phevaluator_int(&local_board_storage_for_this_call[3]);
    if (local_board_storage_for_this_call.size() > 4) c5 = _convert_card_to_phevaluator_int(&local_board_storage_for_this_call[4]);

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRankWithPotential(string)] Hole: " << twoHoleCardsStr << ", Board: " << boardCardsStr << ", Auto-detected stage based on board size: " << local_board_storage_for_this_call.size() << std::endl;
    std::cout << "  Converted to phevaluator format - h1: " << h1 << ", h2: " << h2
              << ", c1: " << c1 << ", c2: " << c2 << ", c3: " << c3 << ", c4: " << c4 << ", c5: " << c5 << std::endl;
    #endif

    // Collect all cards into array for new API
    std::vector<int> all_cards;
    all_cards.push_back(h1);
    all_cards.push_back(h2);
    if (c1 != -1) all_cards.push_back(c1);
    if (c2 != -1) all_cards.push_back(c2);
    if (c3 != -1) all_cards.push_back(c3);
    if (c4 != -1) all_cards.push_back(c4);
    if (c5 != -1) all_cards.push_back(c5);

    // Call our new potential evaluator
    int potential_rank = evaluate_holdem_with_potential(all_cards.data(), all_cards.size());

    // To maintain backward compatibility with a system expecting rank (lower is better),
    // we convert strength back to a pseudo-rank.
    // A simple inversion is sufficient: Rank = MaxStrength - CurrentStrength
    return 1000000 - potential_rank;
}

// New overload for vector<Card*> board cards
int PokerEnv::getHandRankWithPotential(int64_t rangeIdx, const std::vector<Card*>& board_cards) const {
    auto it_idx_to_range = m_idxToRangeLut.find(rangeIdx);
    if (it_idx_to_range == m_idxToRangeLut.end()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR PokerEnv::getHandRankWithPotential(vector)] Invalid rangeIdx provided: " << rangeIdx << std::endl;
        #endif
        return 0; // Invalid rangeIdx
    }

    // Get hole cards from range index
    int card1_1d = it_idx_to_range->second.first;
    int card2_1d = it_idx_to_range->second.second;

    // Convert to Card objects
    Card::Suit card1_suit = static_cast<Card::Suit>(card1_1d / 13);
    Card::CardValue card1_value = static_cast<Card::CardValue>(card1_1d % 13);
    Card::Suit card2_suit = static_cast<Card::Suit>(card2_1d / 13);
    Card::CardValue card2_value = static_cast<Card::CardValue>(card2_1d % 13);

    std::vector<Card> local_hole_cards_storage;
    local_hole_cards_storage.emplace_back(card1_suit, card1_value);
    local_hole_cards_storage.emplace_back(card2_suit, card2_value);

    // Convert cards to phevaluator format
    int h1 = _convert_card_to_phevaluator_int(&local_hole_cards_storage[0]);
    int h2 = _convert_card_to_phevaluator_int(&local_hole_cards_storage[1]);

    // Convert board cards to phevaluator format
    int c1 = -1, c2 = -1, c3 = -1, c4 = -1, c5 = -1;
    if (board_cards.size() > 0 && board_cards[0]) c1 = _convert_card_to_phevaluator_int(board_cards[0]);
    if (board_cards.size() > 1 && board_cards[1]) c2 = _convert_card_to_phevaluator_int(board_cards[1]);
    if (board_cards.size() > 2 && board_cards[2]) c3 = _convert_card_to_phevaluator_int(board_cards[2]);
    if (board_cards.size() > 3 && board_cards[3]) c4 = _convert_card_to_phevaluator_int(board_cards[3]);
    if (board_cards.size() > 4 && board_cards[4]) c5 = _convert_card_to_phevaluator_int(board_cards[4]);

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandRankWithPotential(vector)] rangeIdx: " << rangeIdx
              << ", Hole: " << local_hole_cards_storage[0].toString() << " " << local_hole_cards_storage[1].toString()
              << ", Board size: " << board_cards.size() << ", Auto-detected stage based on board size: " << board_cards.size() << std::endl;
    #endif

    // Collect all cards into array for new API
    std::vector<int> all_cards;
    all_cards.push_back(h1);
    all_cards.push_back(h2);
    if (c1 != -1) all_cards.push_back(c1);
    if (c2 != -1) all_cards.push_back(c2);
    if (c3 != -1) all_cards.push_back(c3);
    if (c4 != -1) all_cards.push_back(c4);
    if (c5 != -1) all_cards.push_back(c5);

    // Call our potential evaluator (stage auto-determined)
    int potential_rank = evaluate_holdem_with_potential(all_cards.data(), all_cards.size());

    // Invert strength to pseudo-rank for backward compatibility
    return 1000000 - potential_rank;
}

// ================================
// 2-Card Hand Value Evaluation Methods
// ================================

int PokerEnv::getHandValuebyCard(const Card* card1, const Card* card2) const {
    if (!card1 || !card2) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyCard] Invalid card pointers provided" << std::endl;
        #endif
        return 169; // Return worst possible rank (weakest hand)
    }

    // Convert cards to phevaluator format
    int card1_int = _convert_card_to_phevaluator_int(card1);
    int card2_int = _convert_card_to_phevaluator_int(card2);

    if (card1_int == -1 || card2_int == -1) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyCard] Failed to convert cards to phevaluator format" << std::endl;
        #endif
        return 169; // Return worst possible rank
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandValuebyCard] Card1: " << card1->toString()
              << " (pheval: " << card1_int << "), Card2: " << card2->toString()
              << " (pheval: " << card2_int << ")" << std::endl;
    #endif

    // Call evaluate_2cards from evaluator_extended.c
    int hand_value = evaluate_2cards(card1_int, card2_int);

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandValuebyCard] Hand value: " << hand_value << std::endl;
    #endif

    return hand_value;
}

int PokerEnv::getHandValuebyPlayer(int playerId) const {
    if (playerId < 0 || playerId >= N_SEATS || !players[playerId]) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyPlayer] Invalid playerId: " << playerId << std::endl;
        #endif
        return 169; // Return worst possible rank
    }

    const PokerPlayer* player = players[playerId];
    if (player->hand.size() != 2) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyPlayer] Player " << playerId
                  << " does not have exactly 2 hole cards (has " << player->hand.size() << ")" << std::endl;
        #endif
        return 169; // Return worst possible rank
    }

    const Card* card1 = player->hand[0];
    const Card* card2 = player->hand[1];


    // Collect all cards into array for new API
    std::vector<int> all_cards;
    all_cards.push_back(_convert_card_to_phevaluator_int(card1));
    all_cards.push_back(_convert_card_to_phevaluator_int(card2));

    // Call our potential evaluator (returns strength, higher is better)
    int potential_strength = evaluate_2cards(all_cards[0], all_cards[1]);

    // Invert strength to pseudo-rank for backward compatibility
    return potential_strength;
}

int PokerEnv::getHandValuebyString(const std::string& twoCardsStr) const {
    if (twoCardsStr.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyString] Empty card string provided" << std::endl;
        #endif
        return 169; // Return worst possible rank
    }

    std::string cardStr1, cardStr2;
    size_t space_pos = twoCardsStr.find(' ');

    if (space_pos != std::string::npos) {
        // Space-separated format: "5s 6s"
        cardStr1 = twoCardsStr.substr(0, space_pos);
        size_t first_char_card2 = twoCardsStr.find_first_not_of(' ', space_pos);
        if (first_char_card2 != std::string::npos) {
            cardStr2 = twoCardsStr.substr(first_char_card2);
        } else {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR getHandValuebyString] Malformed space-separated string: " << twoCardsStr << std::endl;
            #endif
            return 169;
        }
    } else {
        // Concatenated format: "5s6s" or "5♠6♠"
        bool found_split = false;

        // Try different split positions
        for (size_t len_card1 = 2; len_card1 <= 5 && len_card1 < twoCardsStr.length(); ++len_card1) {
            std::string potential_card1 = twoCardsStr.substr(0, len_card1);
            std::string potential_card2 = twoCardsStr.substr(len_card1);

            if (potential_card2.length() < 2 || potential_card2.length() > 5) {
                continue; // Invalid length for second card
            }

            try {
                // Try to parse both cards to validate the split
                parseCardStringInternal(potential_card1);
                parseCardStringInternal(potential_card2);

                // If both parsed successfully, use this split
                cardStr1 = potential_card1;
                cardStr2 = potential_card2;
                found_split = true;
                break;
            } catch (const std::invalid_argument&) {
                // This split was invalid, try next length
                continue;
            }
        }

        if (!found_split) {
            #ifdef DEBUG_POKER_ENV
            std::cerr << "[ERROR getHandValuebyString] Could not parse concatenated string: " << twoCardsStr << std::endl;
            #endif
            return 169;
        }
    }

    if (cardStr1.empty() || cardStr2.empty()) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyString] One or both card strings empty after parsing: " << twoCardsStr << std::endl;
        #endif
        return 169;
    }

    #ifdef DEBUG_POKER_ENV
    std::cout << "[DEBUG getHandValuebyString] Parsed '" << twoCardsStr
              << "' into: card1='" << cardStr1 << "', card2='" << cardStr2 << "'" << std::endl;
    #endif

    try {
        // Parse the card strings
        auto parsedCard1 = parseCardStringInternal(cardStr1);
        auto parsedCard2 = parseCardStringInternal(cardStr2);

        // Create temporary Card objects
        Card card1_obj(parsedCard1.second, parsedCard1.first);
        Card card2_obj(parsedCard2.second, parsedCard2.first);

        // Collect all cards into array for new API
        std::vector<int> all_cards;
        all_cards.push_back(_convert_card_to_phevaluator_int(&card1_obj));
        all_cards.push_back(_convert_card_to_phevaluator_int(&card2_obj));

        // Call our potential evaluator (returns strength, higher is better)
        int potential_strength = evaluate_holdem_with_potential(all_cards.data(), all_cards.size());

        // Invert strength to pseudo-rank for backward compatibility (adjusted for 0-10000 range)
        return 10000 - potential_strength;
    } catch (const std::invalid_argument& e) {
        #ifdef DEBUG_POKER_ENV
        std::cerr << "[ERROR getHandValuebyString] Exception parsing cards '"
                  << cardStr1 << "', '" << cardStr2 << "': " << e.what() << std::endl;
        #endif
        return 169; // Return worst possible rank on error
    }
}

// 新增：获取玩家手牌索引的函数
std::vector<int> PokerEnv::getHandByPid(int playerId) const {
    if (playerId < 0 || playerId >= N_SEATS) {
        return {-1, -1}; // 无效的玩家ID
    }

    PokerPlayer* p = players[playerId];
    if (!p || !p->hand[0] || !p->hand[1]) {
        return {-1, -1}; // 玩家或手牌无效
    }

    // 应用花色同构映射获取标准手牌
    std::vector<int> canonical_suit_map = _getCanonicalSuitMap_static(getCommunityCards());

    int rank1 = p->hand[0]->getValue();
    int suit1 = canonical_suit_map[p->hand[0]->getSuit()];
    int canon_card1_idx = rank1 * 4 + suit1;

    int rank2 = p->hand[1]->getValue();
    int suit2 = canonical_suit_map[p->hand[1]->getSuit()];
    int canon_card2_idx = rank2 * 4 + suit2;

    return {canon_card1_idx, canon_card2_idx};
}
// 新增：获取玩家手牌索引的函数
std::vector<std::vector<int>> PokerEnv::getAllHands() const {
    std::vector<std::vector<int>> all_hands;
    all_hands.reserve(N_SEATS);
    for (int i = 0; i < N_SEATS; i++) {
        // More robust check before calling getHandByPid
        if (players[i] && !players[i]->hand.empty() && players[i]->hand[0] && players[i]->hand[1]) {
            all_hands.push_back(getHandByPid(i));
        } else {
            all_hands.push_back({-1, -1});
        }
    }
    return all_hands;
}

// 新增：获取当前玩家需要跟注的金额
int PokerEnv::get_call_amount() const {
    int playerId = currentPlayer;
    if (playerId < 0 || playerId >= N_SEATS) {
        // Handle invalid player ID, perhaps throw an error or return a specific value
        // For now, returning 0 as a safe default, but logging an error might be better.
        // std::cerr << "Error: Invalid player ID " << playerId << " in get_call_amount." << std::endl;
        return 0;
    }
    const PokerPlayer* player = players[playerId];
    if (!player) {
        // Should not happen if playerId is valid
        return 0;
    }

    int maxBetOnTable = _getBiggestBetOutThereAkaTotalToCall();
    int playerCurrentBet = player->currentBet;
    int callAmount = maxBetOnTable - playerCurrentBet;

    // Call amount cannot be negative
    if (callAmount < 0) {
        callAmount = 0;
    }

    // Call amount cannot exceed player's stack
    if (callAmount > player->stack) { // Changed stackSize to stack
        callAmount = player->stack;   // Changed stackSize to stack
    }

    return callAmount;
}

// 测试函数：测试带有手牌和公共牌设置的reset函数
void test_reset_with_cards(PokerEnv* env) {
    printf("开始测试reset函数的手牌和公共牌设置功能...\n");

    // 测试用例1: Preflop测试
    printf("\n=== 测试用例1: Preflop (无公共牌) ===\n");
    std::vector<std::vector<int>> hole_cards = {
        {0, 13},   // Player 0: As (黑桃A), Kh (红桃K)
        {1, 14},   // Player 1: 2s (黑桃2), Ah (红桃A)
        {25, 38}   // Player 2: Kd (方块K), Qs (黑桃Q)
    };
    std::vector<int> board_cards = {}; // 无公共牌

    auto obs = env->reset(false, hole_cards, board_cards);
    printf("当前轮次: %d (应该是PREFLOP=0)\n", env->getCurrentRound());
    printf("当前玩家: %d\n", env->getCurrentPlayer());

    // 检查手牌设置
    for (int i = 0; i < 3; i++) {
        auto player_hand = env->getPlayerHand_py(i);
        printf("玩家 %d 手牌: (%d,%d), (%d,%d)\n", i,
               std::get<0>(player_hand[0]), std::get<1>(player_hand[0]),
               std::get<0>(player_hand[1]), std::get<1>(player_hand[1]));
    }

    // 测试用例2: Flop测试
    printf("\n=== 测试用例2: Flop (3张公共牌) ===\n");
    board_cards = {2, 15, 28}; // 3s, 3h, 3d (三条3)

    obs = env->reset(false, hole_cards, board_cards);
    printf("当前轮次: %d (应该是FLOP=1)\n", env->getCurrentRound());

    auto community_cards = env->getCommunityCards_py();
    printf("公共牌: ");
    for (size_t i = 0; i < community_cards.size(); i++) {
        printf("%d ", community_cards[i]);
    }
    printf("\n");

    // 测试用例3: Turn测试
    printf("\n=== 测试用例3: Turn (4张公共牌) ===\n");
    board_cards = {2, 15, 28, 41}; // 3s, 3h, 3d, 3c (四条3)

    obs = env->reset(false, hole_cards, board_cards);
    printf("当前轮次: %d (应该是TURN=2)\n", env->getCurrentRound());

    community_cards = env->getCommunityCards_py();
    printf("公共牌: ");
    for (size_t i = 0; i < community_cards.size(); i++) {
        printf("%d ", community_cards[i]);
    }
    printf("\n");

    // 测试用例4: River测试
    printf("\n=== 测试用例4: River (5张公共牌) ===\n");
    board_cards = {2, 15, 28, 41, 12}; // 3s, 3h, 3d, 3c, Ks (四条3带K)

    obs = env->reset(false, hole_cards, board_cards);
    printf("当前轮次: %d (应该是RIVER=3)\n", env->getCurrentRound());

    community_cards = env->getCommunityCards_py();
    printf("公共牌: ");
    for (size_t i = 0; i < community_cards.size(); i++) {
        printf("%d ", community_cards[i]);
    }
    printf("\n");

    // 测试用例5: 错误输入测试
    printf("\n=== 测试用例5: 错误输入测试 ===\n");
    std::vector<std::vector<int>> invalid_hole_cards = {
        {-1, 52},   // 无效索引
        {100, 200}  // 超出范围
    };
    std::vector<int> invalid_board_cards = {-5, 60, 100}; // 无效索引

    obs = env->reset(false, invalid_hole_cards, invalid_board_cards);
    printf("错误输入处理完成，游戏仍能正常进行\n");

    printf("\n所有测试用例完成！\n");
}

// 便于从外部调用的C接口测试函数
extern "C" void test_pokerenv_reset_with_cards() {
    printf("正在创建PokerEnv测试实例...\n");

    // 创建基本配置
    nlohmann::json config;
    config["mode_settings"]["is_evaluating"] = true;

    std::vector<float> bet_sizes = {0.326f, 0.5f, 0.618f, 0.832f, 1.0f, 2.0f};

    // 创建PokerEnv实例
    PokerEnv env(config, 6, bet_sizes, false, 50, 100, 0, 20000);

    // 运行测试
    test_reset_with_cards(&env);

    printf("测试完成！\n");
}
// ===================================================================
// Card Isomorphism Helper (Static)
// ===================================================================
// Creates a canonical mapping for suits to handle isomorphisms.
// This makes the learned strategy invariant to specific suit combinations.
// It's a static helper to avoid modifying the class header.
std::vector<int> _getCanonicalSuitMap_static(const std::vector<Card*>& community_cards) {
    // 1. Create a key for the cache from the board cards.
    std::vector<int> board_ints;
    for (const auto* card : community_cards) {
        if (card) { // FIX: Add null check
            board_ints.push_back(card->getCardInt());
        }
    }
    // Sort to make the key independent of card order on the board
    std::sort(board_ints.begin(), board_ints.end());
    int64_t board_long = Card::boardInts2long(board_ints);

    // 2. Check cache first.
    auto it = suit_map_cache.find(board_long);
    if (it != suit_map_cache.end()) {
        return it->second;
    }

    // 3. If not in cache, compute the map.
    const int NUM_SUITS = 4;

    struct SuitInfo {
        int original_suit;
        int count;
        uint16_t rank_mask; // Bitmask of ranks for this suit on the board

        bool operator<(const SuitInfo& other) const {
            if (count != other.count) {
                return count > other.count; // Higher count first
            }
            if (rank_mask != other.rank_mask) {
                return rank_mask > other.rank_mask; // Higher ranks first
            }
            return original_suit < other.original_suit; // Stable sort
        }
    };

    std::vector<SuitInfo> suit_infos(NUM_SUITS);
    for (int i = 0; i < NUM_SUITS; ++i) {
        suit_infos[i] = {i, 0, 0};
    }

    for (const auto* card : community_cards) {
        if (card) { // FIX: Add null check
            suit_infos[card->getSuit()].count++;
            suit_infos[card->getSuit()].rank_mask |= (1 << card->getValue());
        }
    }

    std::sort(suit_infos.begin(), suit_infos.end());

    std::vector<int> canonical_suit_map(NUM_SUITS);
    for (int i = 0; i < NUM_SUITS; ++i) {
        canonical_suit_map[suit_infos[i].original_suit] = i;
    }

    // 4. Store in cache and return.
    suit_map_cache[board_long] = canonical_suit_map;
    return canonical_suit_map;
}


void PokerEnv::printState() const {
    std::cout << "--- PokerEnv State ---" << std::endl;
    std::cout << "Current Player: " << currentPlayer << std::endl;
    std::cout << "Current Round: " << currentRound << std::endl;
    std::cout << "Main Pot: " << mainPot << std::endl;
    std::cout << "Side Pots: ";
    for (int pot : sidePots) std::cout << pot << " ";
    std::cout << std::endl;
    std::cout << "Current Bet: " << getCurrentBet() << std::endl;
    std::cout << "Current Side Pots: ";
    for (int pot : currentSidePots) std::cout << pot << " ";
    std::cout << std::endl;
    std::cout << "Last Action: " << lastAction_member[0] << " (" << lastAction_member[1] << ")" << std::endl;
    std::cout << "Last Raiser: " << lastRaiser << std::endl;
    std::cout << "Number of Raises This Round: " << nRaisesThisRound << std::endl;
    std::cout << "Number of Actions This Episode: " << nActionsThisEpisode << std::endl;
    std::cout << "Hand Is Over: " << handIsOver << std::endl;
    std::cout << "Bet Sizes List As Fraction of Pot: ";
    for (float bet : betSizesListAsFracOfPot) std::cout << bet << " ";
    std::cout << std::endl;
    std::cout << "Uniform Action Interpolation Member: " << uniformActionInterpolation_member << std::endl;
    std::cout << "Capped Raise Member: " << (cappedRaise_member.happenedThisRound ? "True" : "False") << ", Raised by: " << cappedRaise_member.playerThatRaised << ", Cannot Reopen: " << cappedRaise_member.playerThatCantReopen << std::endl;
    std::cout << "First Action No Call: " << FIRST_ACTION_NO_CALL << std::endl;
    std::cout << "Fixed Limit Game: " << IS_FIXED_LIMIT_GAME << std::endl;
    std::cout << "Max Number of Raises Per Round: ";
    for (int max_raises : MAX_N_RAISES_PER_ROUND) std::cout << max_raises << " ";
    std::cout << std::endl;
    std::cout << "Max Rounds Per Hand: " << max_rounds_per_hand << std::endl;
    std::cout << "Number of Active Players Not Folded: " << getNumActivePlayersNotFolded() << std::endl;
    std::cout << std::endl;
}

int PokerEnv::turnBBtoActionInt(float bbMultiplier) {
    // 处理特殊情况
    if (bbMultiplier <= 0.0f) {
        return FOLD; // 0
    }

    // 如果是跟注大小或更小，返回CHECK_CALL
    float callAmount = static_cast<float>(_getBiggestBetOutThereAkaTotalToCall());
    float bbSize = static_cast<float>(BIG_BLIND);
    if (bbMultiplier * bbSize <= callAmount + 0.1f) { // 添加小的容错范围
        return CHECK_CALL; // 1
    }

    // 检查当前玩家是否有效
    if (currentPlayer < 0 || currentPlayer >= N_SEATS || !players[currentPlayer]) {
        return CHECK_CALL; // 默认返回跟注
    }

    PokerPlayer* player = players[currentPlayer];
    float targetAmount = bbMultiplier * bbSize;

    // 寻找最接近目标下注金额的action
    int bestAction = CHECK_CALL;
    float bestDiff = std::numeric_limits<float>::max();

    // 遍历所有可能的加注action
    for (int a = 2; a < N_ACTIONS; ++a) {
        if (a - 2 < 0 || static_cast<size_t>(a - 2) >= betSizesListAsFracOfPot.size()) {
            break;
        }

        float fraction = betSizesListAsFracOfPot[a - 2];
        int raiseAmount = getFractionOfPotRaise(fraction, player);
        float raiseDiff = std::abs(static_cast<float>(raiseAmount) - targetAmount);

        if (raiseDiff < bestDiff) {
            bestDiff = raiseDiff;
            bestAction = a;
        }

        // 如果找到了非常接近的匹配，可以提前退出
        if (raiseDiff < bbSize * 0.1f) { // 容错范围为0.1个大盲
            break;
        }
    }

    return bestAction;
}

// 实现手牌强度和潜力相关函数
float PokerEnv::getCurrentPlayerInitialHandStrength() const {
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        return _initialHandStrengthCache[currentPlayer];
    }
    return 0.0f; // 无效玩家或超出范围
}

float PokerEnv::getCurrentPlayerHandPotential() const {
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        // 为了兼容性，返回equity_vs_all作为浮点数（归一化到0-1）
        holdem_evaluation_t eval = _handPotentialCache[currentPlayer];
        return static_cast<float>(eval.equity_vs_all) / 10000.0f;
    }
    return 0.0f; // 无效玩家或超出范围
}

std::tuple<int, int> PokerEnv::getCurrentPlayerHandMultidimensional() const {
    if (currentPlayer < 0 || currentPlayer >= N_SEATS) {
        // 无效玩家，返回默认值
        return std::make_tuple(5000, 5000);
    }

    // 直接从缓存中获取结果
    holdem_evaluation_t eval = _handPotentialCache[currentPlayer];
    return std::make_tuple(static_cast<int>(eval.equity_vs_all), static_cast<int>(eval.equity_vs_pair_sets));
}

void PokerEnv::_updateHandStrengthAndPotentialForCurrentPlayer() {
    // 为了向后兼容，调用为所有玩家更新的函数，然后设置当前玩家的值
    _updateHandPotentialForAllPlayers();

    // 更新当前玩家的手牌潜力（已经在_updateHandPotentialForAllPlayers中计算）
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        // 为了兼容性，将多维度评估转换为单一浮点值
        holdem_evaluation_t eval = _handPotentialCache[currentPlayer];
        _currentPlayerHandPotential = static_cast<float>(eval.equity_vs_all) / 10000.0f;
    } else {
        _currentPlayerHandPotential = 0.0f;
    }
}

void PokerEnv::_updateHandPotentialForAllPlayers() {
    for (int playerId = 0; playerId < N_SEATS; ++playerId) {
        // 初始化默认值（全0）
        holdem_evaluation_t default_eval = {0, 0};

        if (!players[playerId] || players[playerId]->hand.size() < 2 ||
            !players[playerId]->hand[0] || !players[playerId]->hand[1]) {
            // 无效玩家或手牌不完整，设置为默认值
            _handPotentialCache[playerId] = default_eval;
            continue;
        }

        if (currentRound == PREFLOP) {
            // 翻牌前设置基线值（约50%胜率）
            holdem_evaluation_t preflop_baseline = {0, 0}; // 50% in 0-10000 scale
            _handPotentialCache[playerId] = preflop_baseline;
        } else {
            // 翻牌后计算多维度评估
            if (communityCards.size() > 0) {
                try {
                    // 构建卡牌数组：底牌 + 公共牌
                    std::vector<int> cards;

                    // 添加底牌
                    const Card* card1 = players[playerId]->hand[0];
                    const Card* card2 = players[playerId]->hand[1];
                    cards.push_back(Card::card2int(*card1));
                    cards.push_back(Card::card2int(*card2));

                    // 添加公共牌
                    for (const Card* card : communityCards) {
                        if (card != nullptr) {
                            cards.push_back(Card::card2int(*card));
                        }
                    }

                    // 使用多维度评估函数
                    holdem_evaluation_t evaluation = evaluate_holdem_multidimensional(cards.data(), cards.size());

                    // 验证结果有效性
                    if (evaluation.equity_vs_all > 10000 || evaluation.equity_vs_pair_sets > 10000) {
                        // 如果结果异常，使用基线值
                        holdem_evaluation_t baseline = {5000, 5000};
                        _handPotentialCache[playerId] = baseline;
                    } else {
                        _handPotentialCache[playerId] = evaluation;
                    }

#ifdef DEBUG_POKER_ENV
                    std::cout << "Player " << playerId << " multi-dimensional evaluation: "
                              << "vs_all=" << evaluation.equity_vs_all
                              << ", vs_pair_sets=" << evaluation.equity_vs_pair_sets << std::endl;
#endif
                } catch (const std::exception& e) {
                    // 异常情况使用基线值
                    holdem_evaluation_t baseline = {5000, 5000};
                    _handPotentialCache[playerId] = baseline;
#ifdef DEBUG_POKER_ENV
                    std::cout << "Exception in multi-dimensional evaluation for player " << playerId
                              << ": " << e.what() << std::endl;
#endif
                }
            } else {
                // 无公共牌时使用基线值
                holdem_evaluation_t baseline = {5000, 5000};
                _handPotentialCache[playerId] = baseline;
            }
        }
    }
}

// 私有信息相关方法实现
void PokerEnv::_updatePrivateInfo(int player_id) {
    if (player_id < 0 || player_id >= N_SEATS || !players[player_id]) {
        return;
    }

    PrivateInfo& info = cached_private_info[player_id];

    // 更新Range Index
    info.range_idx = getRangeIdx(player_id);

    // 更新手牌字符串
    if (players[player_id]->hand.size() >= 2 && players[player_id]->hand[0] && players[player_id]->hand[1]) {
        info.hand_string = players[player_id]->hand[0]->toString() + players[player_id]->hand[1]->toString();
    } else {
        info.hand_string = "XX";
    }

    // 更新手牌强度
    // if (player_id < static_cast<int>(_initialHandStrengthCache.size())) {
    //     info.hand_strength = _initialHandStrengthCache[player_id];
    // } else {
    //     info.hand_strength = 0.0f;
    // }
    int handRank = getHandValuebyPlayer(player_id);
    info.hand_strength = static_cast<float>(170 - handRank) / 169.0f;

    // 标记为有效
    info.is_valid = (info.range_idx >= 0);
}

std::vector<float> PokerEnv::_getPrivateObservation(int player_id) {
    if (player_id < 0 || player_id >= N_SEATS) {
        return std::vector<float>(4, 0.0f); // 返回空的私有信息
    }

    // 更新私有信息
    _updatePrivateInfo(player_id);

    // 返回向量化的私有信息
    return cached_private_info[player_id].toVector();
}

std::vector<std::vector<float>> PokerEnv::_calculateCurrentObservationWithPrivateInfo() {
        std::vector<std::vector<float>> all_observations;

    // 获取基础观察向量（公共信息）
    std::vector<float> base_obs = _calculateCurrentObservationSimplified();



    // 为每个玩家生成包含私有信息的观察向量
    for (int i = 0; i < N_SEATS; i++) {
        std::vector<float> player_obs = base_obs;  // 复制基础观察向量

        // 如果debug_obs_flag为true，添加该玩家的私有信息
        if (debug_obs_flag && players[i]) {
            std::vector<float> private_info = _getPrivateObservation(i);
            // 将私有信息添加到观察向量末尾
            player_obs.insert(player_obs.end(), private_info.begin(), private_info.end());
        }

        all_observations.push_back(player_obs);

    }

    return all_observations;
}

// Python接口方法：获取包含私有信息的观察向量（所有玩家）
std::vector<std::vector<float>> PokerEnv::getObservationWithPrivateInfo_py() {
    std::vector<std::vector<float>> all_observations;

    // 获取基础观察向量（公共信息）
    std::vector<float> base_obs = _calculateCurrentObservationSimplified();

    // 为每个玩家生成包含私有信息的观察向量
    for (int i = 0; i < N_SEATS; i++) {
        std::vector<float> player_obs = base_obs;  // 复制基础观察向量

        // 总是添加该玩家的私有信息（不依赖debug_obs_flag）
        if (players[i]) {
            std::vector<float> private_info = _getPrivateObservation(i);
            // 将私有信息添加到观察向量末尾
            player_obs.insert(player_obs.end(), private_info.begin(), private_info.end());
        } else {
            // 如果玩家不存在，添加空的私有信息
            player_obs.insert(player_obs.end(), 4, 0.0f);
        }

        all_observations.push_back(player_obs);
    }

    return all_observations;
}

// Python接口方法：获取所有玩家的手牌字符串
std::vector<std::string> PokerEnv::getAllPlayersHandStrings_py() {
    std::vector<std::string> hand_strings;

    for (int i = 0; i < N_SEATS; i++) {
        if (players[i] && players[i]->hand.size() >= 2 &&
            players[i]->hand[0] && players[i]->hand[1]) {
            // 获取真实的手牌字符串
            std::string hand_str = players[i]->hand[0]->toString() + players[i]->hand[1]->toString();
            hand_strings.push_back(hand_str);
        } else {
            hand_strings.push_back("XX");
        }
    }

    return hand_strings;
}

// Python接口方法：获取所有玩家的私有信息
std::vector<std::vector<float>> PokerEnv::getAllPlayersPrivateInfo_py() {
    std::vector<std::vector<float>> all_private_info;

    for (int i = 0; i < N_SEATS; i++) {
        // 更新该玩家的私有信息
        _updatePrivateInfo(i);

        // 获取该玩家的私有信息
        std::vector<float> player_private_info = _getPrivateObservation(i);
        all_private_info.push_back(player_private_info);
    }

    return all_private_info;
}

// 新增：为Transformer返回分离的观察数据
std::tuple<std::vector<std::vector<float>>, std::vector<float>> PokerEnv::getObservationForTransformer() {
    // === 1. Prepare the State Vector (Fixed-size features) ===
    std::vector<float> state_features;

    // 计算状态特征大小
    int stateFeatureSize = N_SEATS + N_SEATS + 1 + 1; // 当前玩家位置 + 筹码量 + 有效筹码比例 + 未行动玩家比例
    state_features.reserve(stateFeatureSize);

    // 1. 当前玩家位置 (N_SEATS个位置，one-hot编码)
    std::vector<float> currentPlayerPos(N_SEATS, 0.0f);
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        // 计算当前玩家相对于按钮的位置
        int relativePos = (currentPlayer - buttonPos + N_SEATS) % N_SEATS;
        if (relativePos >= 0 && relativePos < N_SEATS) {
            currentPlayerPos[relativePos] = 1.0f;
        }
    }
    state_features.insert(state_features.end(), currentPlayerPos.begin(), currentPlayerPos.end());

    // 2. 每个玩家的筹码量（相对于初始筹码量）
    std::vector<float> stackSizes(N_SEATS, 0.0f);
    for (int i = 0; i < N_SEATS; i++) {
        if (players[i] && DEFAULT_STACK_SIZE > 0) {
            // 计算当前总筹码量（包括已下注的筹码）
            int totalChips = players[i]->stack + players[i]->currentBet;
            stackSizes[i] = static_cast<float>(totalChips) / static_cast<float>(DEFAULT_STACK_SIZE);
        }
    }
    state_features.insert(state_features.end(), stackSizes.begin(), stackSizes.end());

    // 3. 有效筹码量与当前底池的比例
    float effectiveStackToPotRatio = 0.0f;
    if (currentPlayer >= 0 && currentPlayer < N_SEATS && players[currentPlayer]) {
        // 使用getPotSize()方法获取当前底池大小，确保一致性
        int currentPot = getPotSize();
        if (currentPot == 0) currentPot = BIG_BLIND; // 避免除零

        // 计算有效筹码量：当前玩家与所有未弃牌对手之间的最小筹码量
        int currentPlayerTotalChips = players[currentPlayer]->stack + players[currentPlayer]->currentBet;
        int effectiveStack = currentPlayerTotalChips;

        // 找到最小对手筹码量
        for (int i = 0; i < N_SEATS; i++) {
            if (i != currentPlayer && players[i] && !players[i]->folded) {
                int opponentTotalChips = players[i]->stack + players[i]->currentBet;
                effectiveStack = std::min(effectiveStack, opponentTotalChips);
            }
        }

        effectiveStackToPotRatio = static_cast<float>(effectiveStack) / static_cast<float>(currentPot);
    }
    state_features.push_back(effectiveStackToPotRatio);

    // 4. 当前玩家之后还有多少人未行动
    float playersToActRatio = 0.0f;
    if (currentPlayer >= 0 && currentPlayer < N_SEATS) {
        int playersToAct = 0;

        // 从当前玩家的下一个位置开始计算
        for (int i = 1; i < N_SEATS; i++) {
            int nextPlayerIdx = (currentPlayer + i) % N_SEATS;
            if (players[nextPlayerIdx] && !players[nextPlayerIdx]->folded && !players[nextPlayerIdx]->isAllin) {
                playersToAct++;
            }
        }

        // 归一化：除以最大可能的未行动玩家数（N_SEATS - 1）
        playersToActRatio = static_cast<float>(playersToAct) / static_cast<float>(N_SEATS - 1);
    }
    state_features.push_back(playersToActRatio);

    // === 2. Prepare the Sequence Data (Variable-length action history) ===
    std::vector<std::vector<float>> sequence_features;
    sequence_features.reserve(actionHistory.size());

    const int actionFeatureSize = N_SEATS + N_ACTIONS + 1; // 玩家位置 + 固定动作向量 + 下注倍数
    ALL_FEATURE_SIZE = stateFeatureSize + actionFeatureSize;

    for (const auto& record : actionHistory) {
        // Create a feature vector for THIS action only
        std::vector<float> single_action_feature_vector;
        single_action_feature_vector.reserve(actionFeatureSize);

        // 玩家位置向量 (N_SEATS)
        std::vector<float> playerPos(N_SEATS, 0.0f);

        // 固定大小的动作向量 (N_ACTIONS)
        std::vector<float> actionTaken(N_ACTIONS, 0.0f);

        // 下注倍数 (1)
        float betMultiplier = 0.0f;

        // 设置玩家位置（相对于按钮位置）
        if (record.playerId >= 0 && record.playerId < N_SEATS) {
            // 计算相对位置：button=0, sb=1, bb=2, utg=3, ...
            int relativePos = (record.playerId - buttonPos + N_SEATS) % N_SEATS;
            if (relativePos >= 0 && relativePos < N_SEATS) {
                playerPos[relativePos] = 1.0f;
            }
        }

        // 直接使用记录的actionInt，避免重新计算
        int actionIndex = record.actionInt;
        if (actionIndex >= 0 && actionIndex < N_ACTIONS) {
            actionTaken[actionIndex] = 1.0f;
        }

        // 统一的下注倍数计算：下注额 / 底池大小
        if (record.betAmount > 0) {
            int historicalPot = record.potAtActionTime > 0 ? record.potAtActionTime : BIG_BLIND;
            // 对所有动作类型（跟注、加注）都使用相对于底池的比例
            betMultiplier = static_cast<float>(record.betAmount) / static_cast<float>(historicalPot);
        } else {
            betMultiplier = 0.0f;
        }

        // 构建单个动作的特征向量
        single_action_feature_vector.insert(single_action_feature_vector.end(), playerPos.begin(), playerPos.end());
        single_action_feature_vector.insert(single_action_feature_vector.end(), actionTaken.begin(), actionTaken.end());
        single_action_feature_vector.push_back(betMultiplier);

        sequence_features.push_back(single_action_feature_vector);
    }

    // === 3. Return both parts in a tuple ===
    return std::make_tuple(sequence_features, state_features);
}

// ActionRecord的toString方法实现
std::string PokerEnv::ActionRecord::toString() const {
    std::stringstream ss;

    // 轮次名称映射
    std::string roundNames[] = {"PREFLOP", "FLOP", "TURN", "RIVER"};
    std::string roundName = (round >= 0 && round < 4) ? roundNames[round] : "UNKNOWN";

    // 动作类型名称映射
    std::string actionNames[] = {"FOLD", "CHECK/CALL", "BET/RAISE"};
    std::string actionName = (actionType >= 0 && actionType < 3) ? actionNames[actionType] : "UNKNOWN";

    ss << "Player" << playerId << " " << actionName;

    if (actionType == BET_RAISE && betAmount > 0) {
        ss << " " << betAmount;
    }

    ss << " (Round: " << roundName
       << ", Pot: " << potAtActionTime
       << ", Stack: " << playerStackAtActionTime
       << ", ActionInt: " << actionInt << ")";

    return ss.str();
}
