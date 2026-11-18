use std::ptr;

use super::*;

struct Storage {
    words: [u64; 8],
    chunk: sys::spa_chunk,
    header: sys::spa_meta_header,
    data: sys::spa_data,
    meta: sys::spa_meta,
}

impl Storage {
    fn new() -> Self {
        Self {
            words: [0; 8],
            chunk: sys::spa_chunk {
                offset: 0,
                size: 16,
                stride: 8,
                flags: 0,
            },
            header: Header {
                flags: 0,
                seq: 43,
                pts: 9000,
                dts_offset: 0,
                offset: 0,
            }
            .into(),
            // The pointers and type are assigned when attaching a buffer.
            data: unsafe { std::mem::zeroed() },
            meta: unsafe { std::mem::zeroed() },
        }
    }

    fn buffer(&mut self) -> sys::spa_buffer {
        self.data.type_ = sys::SPA_DATA_MemPtr;
        self.data.maxsize = size_of::<[u64; 8]>() as u32;
        self.data.data = self.words.as_mut_ptr().cast();
        self.data.chunk = ptr::from_mut(&mut self.chunk);
        self.meta.type_ = sys::SPA_META_Header;
        self.meta.size = size_of::<sys::spa_meta_header>() as u32;
        self.meta.data = ptr::from_mut(&mut self.header).cast();
        sys::spa_buffer {
            n_metas: 1,
            n_datas: 1,
            metas: ptr::from_mut(&mut self.meta),
            datas: ptr::from_mut(&mut self.data),
        }
    }
}

#[test]
fn validated_frames_preserve_padded_rows_and_publication_header() {
    let format = Format::f32_image("org.pipewireao.test/1", 2, 2, None).unwrap();
    let (mut input, mut output) = (Storage::new(), Storage::new());
    input.chunk = sys::spa_chunk {
        offset: 72,
        size: 24,
        stride: 16,
        flags: 0,
    };
    output.chunk.stride = 16;
    let input_buffer = input.buffer();
    let mut output_buffer = output.buffer();

    // Each record and payload remains in its own live storage for both borrows.
    let source = unsafe { InputFrame::new(&input_buffer, &format) }.unwrap();
    let mut destination = unsafe { OutputFrame::new(&mut output_buffer, &format) }.unwrap();
    assert!(!overlaps(&source, &destination));
    assert_eq!(
        (source.lines(), source.stride(), source.bytes().len()),
        (2, 16, 24)
    );
    assert_eq!((destination.lines(), destination.stride()), (2, 16));
    assert_eq!(source.u8().1, 16);
    assert_eq!(source.u16().unwrap().1, 8);
    assert_eq!(source.u32().unwrap().1, 4);
    assert_eq!(source.f32().unwrap().1, 4);
    assert_eq!(source.f64().unwrap().1, 2);
    assert_eq!(destination.u16_mut().unwrap().1, 8);
    assert_eq!(destination.f64_mut().unwrap().1, 2);
    assert_eq!(destination.f32_mut().unwrap().1, 4);
    let (bytes, stride) = destination.u8_mut();
    bytes[stride..stride + 8].fill(0x31);
    assert_eq!(&destination.bytes_mut()[16..24], &[0x31; 8]);
    destination.set_metadata(source.metadata()).unwrap();
    destination.set_header(source.header());
    destination.commit();
    assert_eq!(output.header.seq, 43);
    assert_eq!(output.header.pts, 9000);
    assert_eq!(
        (output.chunk.size, output.chunk.stride, output.chunk.offset),
        (24, 16, 0)
    );
}

#[test]
fn frame_views_reject_invalid_spans_and_metadata_before_access() {
    let format = Format::f32_image("org.pipewireao.test/1", 2, 2, None).unwrap();
    let mut storage = Storage::new();
    for (offset, size, stride) in [(60, 16, 0), (0, 15, 8), (0, 16, 4), (0, 16, -8)] {
        storage.chunk = sys::spa_chunk {
            offset,
            size,
            stride,
            flags: 0,
        };
        let buffer = storage.buffer();
        assert!(
            matches!(unsafe { InputFrame::new(&buffer, &format) }, Err(error) if error == -libc::EINVAL)
        );
    }
    storage.chunk = sys::spa_chunk {
        offset: 1,
        size: 16,
        stride: 0,
        flags: 0,
    };
    let buffer = storage.buffer();
    let input = unsafe { InputFrame::new(&buffer, &format) }.unwrap();
    assert_eq!(input.f32(), Err(-libc::EINVAL));

    let mut buffer = storage.buffer();
    storage.data.maxsize = 8;
    assert!(
        matches!(unsafe { OutputFrame::new(&mut buffer, &format) }, Err(error) if error == -libc::ENOSPC)
    );
    let mut buffer = storage.buffer();
    buffer.n_metas = 0;
    assert!(
        matches!(unsafe { OutputFrame::new(&mut buffer, &format) }, Err(error) if error == -libc::EINVAL)
    );
    assert!(
        matches!(unsafe { InputFrame::new(&buffer, &format) }, Err(error) if error == -libc::EINVAL)
    );
}

#[test]
fn forwarding_requires_shared_payload_and_publishes_new_metadata() {
    let format = Format::f32_image("org.pipewireao.test/1", 2, 2, None).unwrap();
    let (mut input, mut output) = (Storage::new(), Storage::new());
    let input_buffer = input.buffer();
    let mut output_buffer = output.buffer();
    // Distinct buffer records remain owned by the test throughout both calls.
    unsafe {
        assert!(!shares_data(&input_buffer, &output_buffer));
        assert!(!forward_frame(&input_buffer, &mut output_buffer, &format, None).unwrap());
        (*output_buffer.datas).data = (*input_buffer.datas).data;
        assert!(shares_data(&input_buffer, &output_buffer));
        let mut header = Header::from(input.header);
        header.seq = 44;
        assert!(forward_frame(&input_buffer, &mut output_buffer, &format, Some(header)).unwrap());
    }
    assert_eq!(output.header.seq, 44);
    assert_eq!((output.chunk.size, output.chunk.stride), (16, 8));
}
