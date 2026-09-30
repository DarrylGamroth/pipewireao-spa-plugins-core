//! Construction and parsing of SPA POD values.

use std::io::Cursor;
use std::mem::size_of;
use std::ptr;

use libspa::param::format::{ElementType, NdArrayFormat, NdArrayLayout};
use libspa::pod::deserialize::PodDeserializer;
use libspa::pod::serialize::PodSerializer;
use libspa::pod::{ChoiceValue, Object, Value};
use libspa::sys;
use libspa::utils::{Choice, ChoiceEnum, ChoiceFlags, Fraction, Id, Rectangle};

use crate::format::{Format, FormatClass, FormatConstraint, Rate};
use crate::pod_bridge;

/// One property within an SPA object POD.
pub use libspa::pod::Property;

fn property(key: u32, value: Value) -> Property {
    Property::new(key, value)
}

/// Constructs an SPA object value.
pub fn object(type_: u32, id: u32, properties: Vec<Property>) -> Value {
    Value::Object(Object {
        type_,
        id,
        properties,
    })
}

fn id(value: u32) -> Value {
    Value::Id(Id(value))
}

pub(crate) fn position_io() -> Value {
    object(
        sys::SPA_TYPE_OBJECT_ParamIO,
        sys::SPA_PARAM_IO,
        vec![
            property(sys::SPA_PARAM_IO_id, id(sys::SPA_IO_Position)),
            property(
                sys::SPA_PARAM_IO_size,
                Value::Int(size_of::<sys::spa_io_position>() as i32),
            ),
        ],
    )
}

fn int_range(default: i32, min: i32, max: i32) -> Value {
    Value::Choice(ChoiceValue::Int(Choice(
        ChoiceFlags::empty(),
        ChoiceEnum::Range { default, min, max },
    )))
}

pub(crate) fn port_param(
    id_: u32,
    index: u32,
    direction: sys::spa_direction,
    constraints: &[FormatConstraint],
    format: Option<&Format>,
) -> Result<Option<Value>, i32> {
    let value = match id_ {
        sys::SPA_PARAM_EnumFormat => {
            let Some(constraint) = constraints.get(index as usize) else {
                return Ok(None);
            };
            format_value(constraint.format(), sys::SPA_PARAM_EnumFormat)
        }
        sys::SPA_PARAM_Format if index == 0 => {
            format_value(format.ok_or(-libc::EIO)?, sys::SPA_PARAM_Format)
        }
        sys::SPA_PARAM_Buffers if index == 0 => buffer_param(format.ok_or(-libc::EIO)?)?,
        sys::SPA_PARAM_Meta if index == 0 => object(
            sys::SPA_TYPE_OBJECT_ParamMeta,
            id_,
            vec![
                property(sys::SPA_PARAM_META_type, id(sys::SPA_META_Header)),
                property(
                    sys::SPA_PARAM_META_size,
                    Value::Int(size_of::<sys::spa_meta_header>() as i32),
                ),
            ],
        ),
        sys::SPA_PARAM_IO => match (direction, index) {
            (_, 0) => object(
                sys::SPA_TYPE_OBJECT_ParamIO,
                id_,
                vec![
                    property(sys::SPA_PARAM_IO_id, id(sys::SPA_IO_Buffers)),
                    property(
                        sys::SPA_PARAM_IO_size,
                        Value::Int(size_of::<sys::spa_io_buffers>() as i32),
                    ),
                ],
            ),
            _ => return Ok(None),
        },
        sys::SPA_PARAM_Format | sys::SPA_PARAM_Buffers | sys::SPA_PARAM_Meta => return Ok(None),
        _ => return Err(-libc::ENOENT),
    };
    Ok(Some(value))
}

fn format_value(format: &Format, object_id: u32) -> Value {
    match format.class {
        FormatClass::Gray8 | FormatClass::Gray16Le | FormatClass::Gray16Be => object(
            sys::SPA_TYPE_OBJECT_Format,
            object_id,
            vec![
                property(sys::SPA_FORMAT_mediaType, id(sys::SPA_MEDIA_TYPE_video)),
                property(sys::SPA_FORMAT_mediaSubtype, id(sys::SPA_MEDIA_SUBTYPE_raw)),
                property(
                    sys::SPA_FORMAT_VIDEO_format,
                    id(match format.class {
                        FormatClass::Gray8 => sys::SPA_VIDEO_FORMAT_GRAY8,
                        FormatClass::Gray16Le => sys::SPA_VIDEO_FORMAT_GRAY16_LE,
                        FormatClass::Gray16Be => sys::SPA_VIDEO_FORMAT_GRAY16_BE,
                        FormatClass::NdArray => unreachable!(),
                    }),
                ),
                property(
                    sys::SPA_FORMAT_VIDEO_size,
                    Value::Rectangle(Rectangle {
                        width: format.width().expect("validated width"),
                        height: format.height().expect("validated height"),
                    }),
                ),
                property(
                    sys::SPA_FORMAT_VIDEO_framerate,
                    Value::Fraction(rate_fraction(format.rate.expect("validated video rate"))),
                ),
            ],
        ),
        FormatClass::NdArray => {
            let mut properties = NdArrayFormat::new(
                ElementType::from_raw(format.element_type),
                format.shape.to_vec(),
                NdArrayLayout::from_raw(format.layout),
                format.rate.map(rate_fraction),
            )
            .expect("validated ndarray format")
            .properties();
            if let Some(schema) = &format.schema {
                properties.push(property(
                    sys::SPA_FORMAT_NDARRAY_schema,
                    Value::String(schema.to_string()),
                ));
            }
            object(sys::SPA_TYPE_OBJECT_Format, object_id, properties)
        }
    }
}

fn rate_fraction(rate: Rate) -> Fraction {
    Fraction {
        num: rate.num,
        denom: rate.denom,
    }
}

fn buffer_param(format: &Format) -> Result<Value, i32> {
    let stride = i32::try_from(format.packed_stride()?).map_err(|_| -libc::EOVERFLOW)?;
    let size = i32::try_from(format.packed_bytes()?).map_err(|_| -libc::EOVERFLOW)?;
    let mem_ptr = 1_i32
        .checked_shl(sys::SPA_DATA_MemPtr)
        .ok_or(-libc::EOVERFLOW)?;
    let mem_fd = 1_i32
        .checked_shl(sys::SPA_DATA_MemFd)
        .ok_or(-libc::EOVERFLOW)?;
    let memory = mem_ptr | mem_fd;
    Ok(object(
        sys::SPA_TYPE_OBJECT_ParamBuffers,
        sys::SPA_PARAM_Buffers,
        vec![
            property(sys::SPA_PARAM_BUFFERS_buffers, int_range(3, 2, 16)),
            property(sys::SPA_PARAM_BUFFERS_blocks, Value::Int(1)),
            property(sys::SPA_PARAM_BUFFERS_size, Value::Int(size)),
            property(sys::SPA_PARAM_BUFFERS_stride, Value::Int(stride)),
            property(
                sys::SPA_PARAM_BUFFERS_dataType,
                Value::Choice(ChoiceValue::Int(Choice(
                    ChoiceFlags::empty(),
                    ChoiceEnum::Flags {
                        default: memory,
                        flags: Vec::new(),
                    },
                ))),
            ),
        ],
    ))
}

fn unique(properties: &[Property], key: u32) -> Result<Option<&Value>, i32> {
    let mut values = properties
        .iter()
        .filter(|property| property.key == key)
        .map(|property| &property.value);
    let value = values.next();
    if values.next().is_some() {
        return Err(-libc::EINVAL);
    }
    Ok(value)
}

pub(crate) fn parse_format(value: Value, constraints: &[FormatConstraint]) -> Result<Format, i32> {
    let Value::Object(object) = value else {
        return Err(-libc::EPROTOTYPE);
    };
    if object.type_ != sys::SPA_TYPE_OBJECT_Format {
        return Err(-libc::EPROTOTYPE);
    }
    let media_type = match unique(&object.properties, sys::SPA_FORMAT_mediaType)? {
        Some(Value::Id(Id(value))) => *value,
        _ => return Err(-libc::EINVAL),
    };
    let media_subtype = match unique(&object.properties, sys::SPA_FORMAT_mediaSubtype)? {
        Some(Value::Id(Id(value))) => *value,
        _ => return Err(-libc::EINVAL),
    };

    let format =
        if media_type == sys::SPA_MEDIA_TYPE_video && media_subtype == sys::SPA_MEDIA_SUBTYPE_raw {
            parse_gray(&object.properties)?
        } else if media_type == sys::SPA_MEDIA_TYPE_application
            && media_subtype == sys::SPA_MEDIA_SUBTYPE_ndarray
        {
            parse_ndarray(&object.properties)?
        } else {
            return Err(-libc::EINVAL);
        };
    format.validate()?;
    if !constraints
        .iter()
        .any(|constraint| constraint.accepts(&format))
    {
        return Err(-libc::EINVAL);
    }
    Ok(format)
}

fn parse_gray(properties: &[Property]) -> Result<Format, i32> {
    let pixel_format = match unique(properties, sys::SPA_FORMAT_VIDEO_format)? {
        Some(Value::Id(Id(value)))
            if matches!(
                *value,
                sys::SPA_VIDEO_FORMAT_GRAY8
                    | sys::SPA_VIDEO_FORMAT_GRAY16_LE
                    | sys::SPA_VIDEO_FORMAT_GRAY16_BE
            ) =>
        {
            *value
        }
        _ => return Err(-libc::EINVAL),
    };
    let size = match unique(properties, sys::SPA_FORMAT_VIDEO_size)? {
        Some(Value::Rectangle(size)) => *size,
        _ => return Err(-libc::EINVAL),
    };
    let rate = match unique(properties, sys::SPA_FORMAT_VIDEO_framerate)? {
        Some(Value::Fraction(rate)) => Rate::new(rate.num, rate.denom)?,
        _ => return Err(-libc::EINVAL),
    };
    match pixel_format {
        sys::SPA_VIDEO_FORMAT_GRAY8 => Format::gray8(size.width, size.height, rate),
        sys::SPA_VIDEO_FORMAT_GRAY16_LE => Format::gray16_le(size.width, size.height, rate),
        sys::SPA_VIDEO_FORMAT_GRAY16_BE => Format::gray16_be(size.width, size.height, rate),
        _ => unreachable!("pixel format was validated above"),
    }
}

fn parse_ndarray(properties: &[Property]) -> Result<Format, i32> {
    let native = NdArrayFormat::from_properties(properties).map_err(|_| -libc::EINVAL)?;
    let schema = match unique(properties, sys::SPA_FORMAT_NDARRAY_schema)? {
        Some(Value::String(schema)) if !schema.is_empty() => Some(schema.clone().into_boxed_str()),
        None => None,
        _ => return Err(-libc::EINVAL),
    };
    Format::ndarray_format(
        native.element_type().as_raw(),
        schema,
        native.shape().to_vec(),
        native.layout().as_raw(),
        native.rate().map(|rate| Rate {
            num: rate.num,
            denom: rate.denom,
        }),
    )
}

/// Returns the properties from a correctly typed Props object.
pub fn parse_props(value: Value) -> Result<Vec<Property>, i32> {
    let Value::Object(object) = value else {
        return Err(-libc::EPROTOTYPE);
    };
    if object.type_ != sys::SPA_TYPE_OBJECT_Props || object.id != sys::SPA_PARAM_Props {
        return Err(-libc::EPROTOTYPE);
    }
    Ok(object.properties)
}

pub(crate) unsafe fn decode(pod: *const sys::spa_pod) -> Result<Value, i32> {
    let storage = unsafe { pod_bridge::unwrap_fixed_pod(pod)? };
    let pod = storage.as_ptr().cast::<sys::spa_pod>();
    let pod = unsafe { pod.as_ref() }.ok_or(-libc::EINVAL)?;
    let length = size_of::<sys::spa_pod>()
        .checked_add(pod.size as usize)
        .ok_or(-libc::EOVERFLOW)?;
    let bytes = unsafe { std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), length) };
    let (remaining, value) =
        PodDeserializer::deserialize_any_from(bytes).map_err(|_| -libc::EINVAL)?;
    if !remaining.is_empty() {
        return Err(-libc::EINVAL);
    }
    Ok(value)
}

pub(crate) unsafe fn with_filtered_pod<R>(
    value: &Value,
    filter: *const sys::spa_pod,
    operation: impl FnOnce(*mut sys::spa_pod) -> R,
) -> Result<Option<R>, i32> {
    let (cursor, _) = PodSerializer::serialize(Cursor::new(Vec::with_capacity(1024)), value)
        .map_err(|_| -libc::EINVAL)?;
    let encoded = cursor.into_inner();
    let mut source_words = vec![0_u64; encoded.len().div_ceil(size_of::<u64>())];
    let source_bytes = unsafe {
        std::slice::from_raw_parts_mut(
            source_words.as_mut_ptr().cast::<u8>(),
            source_words.len() * size_of::<u64>(),
        )
    };
    source_bytes[..encoded.len()].copy_from_slice(&encoded);
    let source = source_words.as_mut_ptr().cast::<sys::spa_pod>();
    if filter.is_null() {
        return Ok(Some(operation(source)));
    }

    let filtered_bytes = encoded.len().saturating_mul(2).max(4096);
    let mut filtered_words = vec![0_u64; filtered_bytes.div_ceil(size_of::<u64>())];
    let mut builder = std::mem::MaybeUninit::<sys::spa_pod_builder>::uninit();
    unsafe {
        sys::spa_pod_builder_init(
            builder.as_mut_ptr(),
            filtered_words.as_mut_ptr().cast(),
            u32::try_from(filtered_words.len() * size_of::<u64>()).map_err(|_| -libc::EOVERFLOW)?,
        );
    }
    let mut builder = unsafe { builder.assume_init() };
    let mut result = ptr::null_mut();
    let status = unsafe { sys::spa_pod_filter(&mut builder, &mut result, source, filter) };
    if status < 0 {
        return Ok(None);
    }
    if result.is_null() {
        return Err(-libc::EIO);
    }
    Ok(Some(operation(result)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use libspa::pod::ValueArray;

    #[test]
    fn fixed_native_ndarray_round_trips() {
        let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        let constraint = FormatConstraint::exact(format.clone());
        let value = format_value(&format, sys::SPA_PARAM_Format);
        let parsed = unsafe {
            with_filtered_pod(&value, ptr::null(), |pod| {
                parse_format(decode(pod).unwrap(), &[constraint]).unwrap()
            })
            .unwrap()
            .unwrap()
        };
        assert_eq!(parsed, format);
    }

    #[test]
    fn fixed_f64_vector_round_trips_with_element_stride() {
        let format = Format::ndarray(
            sys::SPA_ELEMENT_TYPE_F64_LE,
            "org.pipewireao.test-vector/1",
            vec![8],
            None,
        )
        .unwrap();
        assert_eq!(format.packed_bytes(), Ok(64));
        assert_eq!(format.packed_stride(), Ok(8));
        assert_eq!(format.line_count(), Ok(8));
        let constraint = FormatConstraint::exact(format.clone());
        let value = format_value(&format, sys::SPA_PARAM_Format);
        let parsed = unsafe {
            with_filtered_pod(&value, ptr::null(), |pod| {
                parse_format(decode(pod).unwrap(), &[constraint]).unwrap()
            })
            .unwrap()
            .unwrap()
        };
        assert_eq!(parsed, format);
    }

    #[test]
    fn every_standard_fixed_width_element_type_is_admitted() {
        let element_types = [
            ElementType::Bool8,
            ElementType::I8,
            ElementType::U8,
            ElementType::I16Le,
            ElementType::U16Le,
            ElementType::I32Le,
            ElementType::U32Le,
            ElementType::I64Le,
            ElementType::U64Le,
            ElementType::I128Le,
            ElementType::U128Le,
            ElementType::F8E4M3Fn,
            ElementType::F8E4M3Fnuz,
            ElementType::F8E5M2,
            ElementType::F8E5M2Fnuz,
            ElementType::F16Le,
            ElementType::Bf16Le,
            ElementType::F32Le,
            ElementType::F64Le,
            ElementType::F128Le,
            ElementType::ComplexF16Le,
            ElementType::ComplexBf16Le,
            ElementType::ComplexF32Le,
            ElementType::ComplexF64Le,
            ElementType::ComplexF128Le,
        ];
        for element_type in element_types {
            let format = Format::ndarray_with_layout(
                element_type.as_raw(),
                "org.pipewireao.test.matrix/1",
                [3, 5],
                NdArrayLayout::ColumnMajor.as_raw(),
                Some(Rate::new(30, 1).unwrap()),
            )
            .unwrap();
            assert_eq!(
                format.packed_stride(),
                element_type
                    .size()
                    .map(|size| 3 * size)
                    .ok_or(-libc::EINVAL)
            );
            assert_eq!(format.line_count(), Ok(5));
            let constraint = FormatConstraint::exact(format.clone());
            let value = format_value(&format, sys::SPA_PARAM_Format);
            assert_eq!(parse_format(value, &[constraint]), Ok(format));
        }
    }

    #[test]
    fn fixated_choice_is_unwrapped_before_exact_format_validation() {
        let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        let constraint = FormatConstraint::exact(format.clone());
        let Value::Object(mut object) = format_value(&format, sys::SPA_PARAM_Format) else {
            unreachable!();
        };
        let element_type = object
            .properties
            .iter_mut()
            .find(|property| property.key == sys::SPA_FORMAT_NDARRAY_elementType)
            .unwrap();
        element_type.value = Value::Choice(ChoiceValue::Id(Choice(
            ChoiceFlags::empty(),
            ChoiceEnum::None(Id(format.element_type)),
        )));
        let parsed = unsafe {
            with_filtered_pod(&Value::Object(object), ptr::null(), |pod| {
                parse_format(decode(pod).unwrap(), &[constraint]).unwrap()
            })
            .unwrap()
            .unwrap()
        };
        assert_eq!(parsed, format);
    }

    #[test]
    fn unresolved_choice_is_rejected_before_format_validation() {
        let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        let Value::Object(mut object) = format_value(&format, sys::SPA_PARAM_Format) else {
            unreachable!();
        };
        let element_type = object
            .properties
            .iter_mut()
            .find(|property| property.key == sys::SPA_FORMAT_NDARRAY_elementType)
            .unwrap();
        element_type.value = Value::Choice(ChoiceValue::Id(Choice(
            ChoiceFlags::empty(),
            ChoiceEnum::Enum {
                default: Id(sys::SPA_ELEMENT_TYPE_F32_LE),
                alternatives: vec![Id(sys::SPA_ELEMENT_TYPE_F64_LE)],
            },
        )));
        let decoded = unsafe {
            with_filtered_pod(&Value::Object(object), ptr::null(), |pod| decode(pod))
                .unwrap()
                .unwrap()
        };
        assert_eq!(decoded, Err(-libc::EINVAL));
    }

    #[test]
    fn optional_schema_may_be_absent() {
        let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        let Value::Object(mut object) = format_value(&format, sys::SPA_PARAM_Format) else {
            unreachable!();
        };
        object
            .properties
            .retain(|property| property.key != sys::SPA_FORMAT_NDARRAY_schema);
        let expected = Format::ndarray_format(
            sys::SPA_ELEMENT_TYPE_F32_LE,
            None,
            [3, 4],
            sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR,
            None,
        )
        .unwrap();
        assert_eq!(
            parse_format(
                Value::Object(object),
                &[FormatConstraint::exact(expected.clone())]
            ),
            Ok(expected)
        );
    }

    #[test]
    fn wrong_schema_is_rejected_by_exact_constraint() {
        let expected = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        let candidate = Format::f32_image("org.calculon.other/1", 4, 3, None).unwrap();
        let value = format_value(&candidate, sys::SPA_PARAM_Format);
        assert_eq!(
            parse_format(value, &[FormatConstraint::exact(expected)]),
            Err(-libc::EINVAL)
        );
    }

    #[test]
    fn ndarray_shape_is_height_then_width() {
        let format = Format::f32_image("org.calculon.test/1", 4, 3, None).unwrap();
        let Value::Object(object) = format_value(&format, sys::SPA_PARAM_Format) else {
            unreachable!();
        };
        assert_eq!(
            unique(&object.properties, sys::SPA_FORMAT_NDARRAY_shape).unwrap(),
            Some(&Value::ValueArray(ValueArray::Int(vec![3, 4])))
        );
    }
}
