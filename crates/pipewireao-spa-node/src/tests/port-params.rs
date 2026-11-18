use super::*;

const INPUT: sys::spa_direction = sys::SPA_DIRECTION_INPUT;

fn property_value(value: &Value, key: u32) -> &Value {
    let Value::Object(object) = value else {
        panic!("expected an object POD");
    };
    unique(&object.properties, key)
        .unwrap()
        .expect("missing property")
}

#[test]
fn advertises_the_registered_format_buffer_geometry_and_metadata() {
    let format = Format::ndarray_with_layout(
        sys::SPA_ELEMENT_TYPE_U16_LE,
        "org.pipewireao.test/1",
        [3, 7],
        sys::SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
        None,
    )
    .unwrap();
    let constraints = [FormatConstraint::exact(format.clone())];
    let records = [
        sys::spa_meta_requirement {
            type_: sys::SPA_META_AcquisitionIdentity,
            size: size_of::<sys::spa_meta_acquisition_identity>() as u32,
            flags: sys::SPA_META_REQUIREMENT_REQUIRED,
            reserved: 0,
        },
        sys::spa_meta_requirement {
            type_: 0x3fe,
            size: 16,
            flags: 0,
            reserved: 0,
        },
    ];
    let selected =
        |kind, index| port_param(kind, index, INPUT, &constraints, Some(&format), &records);
    let buffers = selected(sys::SPA_PARAM_Buffers, 0).unwrap().unwrap();
    assert_eq!(
        property_value(&buffers, sys::SPA_PARAM_BUFFERS_size),
        &Value::Int(42)
    );
    assert_eq!(
        property_value(&buffers, sys::SPA_PARAM_BUFFERS_stride),
        &Value::Int(6)
    );
    assert_eq!(
        property_value(&buffers, sys::SPA_PARAM_BUFFERS_blocks),
        &Value::Int(1)
    );
    assert_eq!(
        property_value(&buffers, sys::SPA_PARAM_BUFFERS_buffers),
        &Value::Choice(ChoiceValue::Int(Choice(
            ChoiceFlags::empty(),
            ChoiceEnum::Range {
                default: 3,
                min: 2,
                max: 16
            },
        ))),
    );
    let mem_types = (1 << sys::SPA_DATA_MemPtr) | (1 << sys::SPA_DATA_MemFd);
    assert_eq!(
        property_value(&buffers, sys::SPA_PARAM_BUFFERS_dataType),
        &Value::Choice(ChoiceValue::Int(Choice(
            ChoiceFlags::empty(),
            ChoiceEnum::Flags {
                default: mem_types,
                flags: vec![]
            },
        ))),
    );
    for (index, type_, size, required) in [
        (
            0,
            sys::SPA_META_Header,
            size_of::<sys::spa_meta_header>() as u32,
            true,
        ),
        (1, records[0].type_, records[0].size, true),
        (2, records[1].type_, records[1].size, false),
    ] {
        let meta = selected(sys::SPA_PARAM_Meta, index).unwrap().unwrap();
        assert_eq!(property_value(&meta, sys::SPA_PARAM_META_type), &id(type_));
        assert_eq!(
            property_value(&meta, sys::SPA_PARAM_META_size),
            &Value::Int(size as i32)
        );
        assert_eq!(
            property_value(&meta, sys::SPA_PARAM_META_required),
            &Value::Bool(required)
        );
    }

    for kind in [sys::SPA_PARAM_Format, sys::SPA_PARAM_EnumFormat] {
        let value = selected(kind, 0).unwrap().unwrap();
        assert_eq!(parse_format(value, &constraints), Ok(format.clone()));
    }
    let io = selected(sys::SPA_PARAM_IO, 0).unwrap().unwrap();
    assert_eq!(
        property_value(&io, sys::SPA_PARAM_IO_id),
        &id(sys::SPA_IO_Buffers)
    );
    assert_eq!(
        property_value(&io, sys::SPA_PARAM_IO_size),
        &Value::Int(size_of::<sys::spa_io_buffers>() as i32)
    );

    for kind in [
        sys::SPA_PARAM_Format,
        sys::SPA_PARAM_Buffers,
        sys::SPA_PARAM_EnumFormat,
        sys::SPA_PARAM_IO,
    ] {
        assert_eq!(selected(kind, 1), Ok(None));
    }
    for kind in [sys::SPA_PARAM_Format, sys::SPA_PARAM_Buffers] {
        assert_eq!(
            port_param(kind, 0, INPUT, &constraints, None, &records),
            Err(-libc::EIO)
        );
    }
    assert_eq!(selected(sys::SPA_PARAM_Meta, 3), Ok(None));
    assert_eq!(selected(sys::SPA_PARAM_Props, 0), Err(-libc::ENOENT));
}

#[test]
fn video_pods_filter_by_carrier_and_reject_missing_duplicate_or_mistyped_fields() {
    let rate = Rate::new(5, 1).unwrap();
    let format = Format::gray16_be(7, 3, rate).unwrap();
    let constraints = [FormatConstraint::exact(format.clone())];
    let advertised = format_value(&format, sys::SPA_PARAM_EnumFormat);
    let same = format_value(&format, sys::SPA_PARAM_EnumFormat);
    let different = format_value(
        &Format::gray16_le(7, 3, rate).unwrap(),
        sys::SPA_PARAM_EnumFormat,
    );

    // The POD pointers stay valid for the duration of their builder callbacks.
    for (filter, expected) in [(same, Some(format)), (different, None)] {
        let selected = unsafe {
            with_filtered_pod(&filter, ptr::null(), |raw_filter| {
                with_filtered_pod(&advertised, raw_filter, |raw| {
                    parse_format(decode(raw).unwrap(), &constraints).unwrap()
                })
                .unwrap()
            })
            .unwrap()
            .unwrap()
        };
        assert_eq!(selected, expected);
    }

    for key in [
        sys::SPA_FORMAT_mediaType,
        sys::SPA_FORMAT_mediaSubtype,
        sys::SPA_FORMAT_VIDEO_format,
        sys::SPA_FORMAT_VIDEO_size,
        sys::SPA_FORMAT_VIDEO_framerate,
    ] {
        let Value::Object(mut fields) = advertised.clone() else {
            unreachable!()
        };
        fields.properties.retain(|field| field.key != key);
        assert_eq!(
            parse_format(Value::Object(fields), &constraints),
            Err(-libc::EINVAL)
        );
    }
    let Value::Object(mut fields) = advertised.clone() else {
        unreachable!()
    };
    fields.properties.push(property(
        sys::SPA_FORMAT_mediaType,
        id(sys::SPA_MEDIA_TYPE_video),
    ));
    assert_eq!(
        parse_format(Value::Object(fields), &constraints),
        Err(-libc::EINVAL)
    );
    let Value::Object(mut fields) = advertised else {
        unreachable!()
    };
    fields
        .properties
        .iter_mut()
        .find(|field| field.key == sys::SPA_FORMAT_mediaSubtype)
        .unwrap()
        .value = id(sys::SPA_MEDIA_SUBTYPE_ndarray);
    assert_eq!(
        parse_format(Value::Object(fields), &constraints),
        Err(-libc::EINVAL)
    );
}

#[test]
fn props_require_the_correct_object_type_and_parameter_id() {
    let fields = vec![property(sys::SPA_PROP_mute, Value::Bool(true))];
    let props = object(
        sys::SPA_TYPE_OBJECT_Props,
        sys::SPA_PARAM_Props,
        fields.clone(),
    );
    assert_eq!(parse_props(props), Ok(fields));
    for value in [
        Value::Bool(true),
        object(sys::SPA_TYPE_OBJECT_Format, sys::SPA_PARAM_Props, vec![]),
        object(sys::SPA_TYPE_OBJECT_Props, sys::SPA_PARAM_Format, vec![]),
    ] {
        assert_eq!(parse_props(value.clone()), Err(-libc::EPROTOTYPE));
        if !matches!(value, Value::Object(ref object) if object.type_ == sys::SPA_TYPE_OBJECT_Format)
        {
            assert_eq!(parse_format(value, &[]), Err(-libc::EPROTOTYPE));
        }
    }
}
