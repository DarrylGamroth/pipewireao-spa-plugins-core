//! Cold metadata profile parsed by the shared SPA-JSON implementation.
use libspa::sys;
use std::ffi::CStr;
#[allow(non_camel_case_types, non_snake_case, dead_code)]
mod ffi {
    include!(concat!(env!("OUT_DIR"), "/metadata.rs"));
}
pub(crate) fn parse(text: &CStr) -> Result<Vec<sys::spa_meta_requirement>, i32> {
    let mut profile = std::mem::MaybeUninit::<ffi::pwao_metadata>::uninit();
    let result = unsafe { ffi::pwao_metadata_parse(profile.as_mut_ptr(), text.as_ptr()) };
    if result < 0 {
        return Err(result);
    }
    let profile = unsafe { profile.assume_init() };
    Ok(profile.records[1..profile.count as usize].to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn explicit_profiles_preserve_required_opaque_layouts() {
        let records = parse(c"[{type=1022 size=8 required=true}]").unwrap();
        assert_eq!(records.len(), 1);
        assert_eq!(
            (records[0].type_, records[0].size, records[0].flags),
            (1022, 8, sys::SPA_META_REQUIREMENT_REQUIRED)
        );
        assert!(parse(c"[]").unwrap().is_empty());
        assert!(parse(c"[{type=1022 size=4065 required=false}]").is_err());
    }
}
