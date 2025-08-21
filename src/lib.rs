// lib.rs
use pyo3::prelude::*;
pub mod game_logic;
pub mod parallel;
pub mod state;
pub mod visualization;

// WebSocket server modules (not exposed to Python)
#[cfg(feature = "websocket")]
pub mod game_server;
#[cfg(feature = "websocket")]
pub mod websocket_server;

/// A Python module implemented in Rust.
#[pymodule]
fn pokers(_py: Python, m: &PyModule) -> PyResult<()> {
    //m.add_function(wrap_pyfunction!(sum_as_string, m)?)?;
    m.add_class::<state::State>()?;
    m.add_class::<state::PlayerState>()?;
    m.add_class::<state::StateStatus>()?;
    m.add_class::<state::BettingRoundContext>()?;
    m.add_class::<state::stage::Stage>()?;
    m.add_class::<state::action::ActionEnum>()?;
    m.add_class::<state::action::Action>()?;
    m.add_class::<state::action::ActionRecord>()?;
    m.add_class::<state::card::Card>()?;
    m.add_function(wrap_pyfunction!(visualization::visualize_state, m)?)?;
    m.add_function(wrap_pyfunction!(visualization::visualize_trace, m)?)?;
    m.add_function(wrap_pyfunction!(parallel::parallel_apply_action, m)?)?;
    Ok(())
}
