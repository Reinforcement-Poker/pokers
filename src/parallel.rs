// parallel.rs
use crate::state::action::Action;
use crate::state::State;
use pyo3::prelude::*;
use rayon::prelude::*;

#[pyfunction]
pub fn parallel_apply_action(mut states: Vec<State>, actions: Vec<Action>) -> Vec<State> {
    states
        .par_iter_mut()
        .zip(actions)
        .for_each(|(s, a)| {
            // apply_action now returns PyResult<()> and modifies in place.
            // We can ignore the result here as errors are stored in state.status.
            s.apply_action_rs(a);
        });
    states
}
