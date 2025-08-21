// state/stage.rs
#![allow(unused)]
#[cfg(test)]
use proptest_derive::Arbitrary;
use pyo3::prelude::*;
use strum_macros::EnumIter;

#[pyclass]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, EnumIter)]
#[repr(u32)]
#[cfg_attr(test, derive(Arbitrary))]
pub enum Stage {
    Preflop = 0,
    Flop = 1,
    Turn = 2,
    River = 3,
    Showdown = 4,
}

#[pymethods]
impl Stage {
    #[staticmethod]
    pub fn from_int(v: u32) -> PyResult<Self> {
        match v {
            0 => Ok(Stage::Preflop),
            1 => Ok(Stage::Flop),
            2 => Ok(Stage::Turn),
            3 => Ok(Stage::River),
            4 => Ok(Stage::Showdown),
            _ => Err(pyo3::exceptions::PyValueError::new_err("invalid Stage value")),
        }
    }

    pub fn value(&self) -> u32 { *self as u32 }
    pub fn __int__(&self) -> u32 { *self as u32 }
    pub fn __hash__(&self) -> u64 { *self as u32 as u64 }
    pub fn __repr__(&self) -> String { format!("Stage::{:?}", self) }
    pub fn name(&self) -> &'static str { match self { Stage::Preflop => "Preflop", Stage::Flop => "Flop", Stage::Turn => "Turn", Stage::River => "River", Stage::Showdown => "Showdown" } }
}
