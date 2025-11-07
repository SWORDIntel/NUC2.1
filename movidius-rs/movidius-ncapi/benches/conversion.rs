//! Benchmark FP16/FP32 conversions

use criterion::{black_box, criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use half::f16;
use movidius_ncapi::conversion::{fp16_to_fp32, fp32_to_fp16};

fn bench_fp32_to_fp16(c: &mut Criterion) {
    let mut group = c.benchmark_group("fp32_to_fp16");

    for size in [100, 1000, 10000, 100000].iter() {
        group.throughput(Throughput::Elements(*size as u64));
        group.bench_with_input(BenchmarkId::from_parameter(size), size, |b, &size| {
            let input = vec![1.0f32; size];
            let mut output = vec![f16::ZERO; size];
            b.iter(|| {
                fp32_to_fp16(black_box(&input), black_box(&mut output));
            });
        });
    }
    group.finish();
}

fn bench_fp16_to_fp32(c: &mut Criterion) {
    let mut group = c.benchmark_group("fp16_to_fp32");

    for size in [100, 1000, 10000, 100000].iter() {
        group.throughput(Throughput::Elements(*size as u64));
        group.bench_with_input(BenchmarkId::from_parameter(size), size, |b, &size| {
            let input = vec![f16::from_f32(1.0); size];
            let mut output = vec![0.0f32; size];
            b.iter(|| {
                fp16_to_fp32(black_box(&input), black_box(&mut output));
            });
        });
    }
    group.finish();
}

criterion_group!(benches, bench_fp32_to_fp16, bench_fp16_to_fp32);
criterion_main!(benches);
