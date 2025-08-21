fn main() {
    // Tell Rust where to find the PokerHandEvaluator library
    println!("cargo:rustc-link-search=native=PokerHandEvaluator/cpp");
    println!("cargo:rustc-link-lib=static=pheval");

    // Include directory for headers
    println!("cargo:include=PokerHandEvaluator/cpp/include");

    // Rebuild if the library changes
    println!("cargo:rerun-if-changed=PokerHandEvaluator/cpp/libpheval.a");
    println!("cargo:rerun-if-changed=PokerHandEvaluator/cpp/include/phevaluator/phevaluator.h");
}
