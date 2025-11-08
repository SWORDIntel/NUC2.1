use criterion::{criterion_group, criterion_main, Criterion};

fn bench_tensor(c: &mut Criterion) {
    c.bench_function("tensor_default", |b| {
        b.iter(movidius_ncapi::TensorDescriptor::default)
    });
}

criterion_group!(benches, bench_tensor);
criterion_main!(benches);
