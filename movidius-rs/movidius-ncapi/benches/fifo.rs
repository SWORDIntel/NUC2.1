use criterion::{criterion_group, criterion_main, Criterion};

fn bench_fifo(c: &mut Criterion) {
    c.bench_function("fifo_create", |b| {
        b.iter(|| {
            let _ = movidius_ncapi::Fifo::create("test", movidius_ncapi::FifoType::HostWo);
        })
    });
}

criterion_group!(benches, bench_fifo);
criterion_main!(benches);
