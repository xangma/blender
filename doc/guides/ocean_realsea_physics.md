# RealSea spectrum implementation version 2

`OceanModifier.spectrum_implementation_version` is read-only: `2` identifies the
variance-preserving `REALSEA_PM` and `REALSEA_JONSWAP` implementation; `1` identifies
the unchanged legacy spectra. Version 2 changes RealSea output and seeded
realizations. Keep original executable/configuration provenance for old datasets.

## Variance convention

The scalar `Sf(f)` is one-sided elevation variance density in m²/Hz. The final
angular distribution `D(f,theta)` integrates to one over the full circle. It is
`1/(2*pi)` when spreading is off. With spreading on, the reference cosine-power
kernel, including its additional wind-cosine-squared factor, is normalized over
its forward support. Its width is controlled by `realsea_spread`; narrowing it
does not intentionally reduce the total spectral variance.

The finite-depth gravity-wave dispersion relation gives

```
omega(k)^2 = g*k*tanh(k*depth)
f = omega/(2*pi)
Sk(kx,kz) = Sf(f) * D(f,theta) * (df/dk) / k
cell_variance(k) = Sk(k) * (2*pi/Lx) * (2*pi/Lz)
```

The conversion has no additional multiplication by `f`. The original MATLAB
amplitude expression and the version-1 Blender exporter included that factor;
those outputs are a historical reference, not an independent validation of this
variance convention.

FFTW's inverse transform is an unnormalized sum. The evolving coefficient is

```
h(k,t) = h0(k)*exp(i*omega*t) + conj(h0(-k))*exp(-i*omega*t)
h0(k) = (normal_1 + i*normal_2) * sqrt(cell_variance(k)/4)
```

The independent standard normal draws have unit variance each, so each `h0`
contributes half the mode variance. The two evolving terms together give
`E[var(height)] = sum(cell_variance)` when the full grid is resolved. Opposite
bins use the actual opposite coefficient, ensuring `h(-k,t)=conj(h(k,t))`.
Even-grid Nyquist axes are suppressed because their propagation direction is
ambiguous on the sampled lattice. The DC coefficient is zero.

The grid uses conventional signed DFT frequencies for RealSea. The legacy
spectra keep their existing frequency indexing, coefficient construction and
peak-height normalization. RealSea never divides by the maximum height of a
particular realization. `use_wave_scale` only enables an optional amplitude
multiplier; disabling it means a multiplier of one.

## Parameters and limits

- Wind is `U19.5` in m/s, measured at 19.5 m height. The existing reference
  conversion `U10=U19.5/1.025` is retained for RealSea JONSWAP and spreading.
- `fetch_jonswap` is in metres. RealSea PM does not use fetch.
- RealSea JONSWAP retains the reference's constant `alpha=0.0081`. It has a
  fetch-dependent peak and configurable peak-enhancement gamma. It is distinct
  from the legacy Blender JONSWAP and does not implement a wave forecast model.
- `sharpen_peak_jonswap` stores gamma divided by ten. A stored zero, the existing
  modifier DNA default, selects gamma 3.3 for RealSea. Positive values map to
  gamma in the supported range 1–6. Legacy spectra's default handling is unchanged.
- Frequency bounds are Hz. Zero disables that bound. Finite negative or reversed
  nonzero bounds are invalid, rather than silently reordered. Spreading width and
  water depth must be positive; wind must be nonnegative, and JONSWAP fetch positive.
- Alignment, reflected-wave damping and Smallest Wave do not affect RealSea.
  Use its frequency limits and spreading controls. Direction applies whenever
  RealSea spreading is enabled, including with legacy Alignment set to zero.

Physical amplitude normalization does not guarantee that a finite patch resolves
all requested energy. Low frequencies need a sufficiently large domain; high
frequencies need sufficiently fine spacing. Compare the discrete spectral sum
and refine both domain and grid before interpreting height/period statistics.
Wave Scale, choppiness, and sampling choices must accompany any measured labels.

## Regression evidence

Run the native, dense modifier checks with the newly built executable:

```sh
blender -b --factory-startup --python-exit-code 1 \
  --python tests/python/physics_ocean_realsea.py
```

The test independently computes deep-water PM density; reconstructs modal
coefficients from evaluated height fields at two times; predicts a third time;
checks modal and spatial variance, opposite bins, frequency bounds and Nyquist
axes; compares patch sizes/resolutions and angular orientations; and exercises
gain, gamma, fetch, revision identity and rejection of reversed bounds.
This is a bounded numerical regression, not field validation of every sea state.
The historical MATLAB parity report belongs to its original revision and does
not establish version-2 correctness.

For the statistical FFT construction and conjugate symmetry, see Jerry
Tessendorf, [Simulating Ocean Water](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2004.pdf),
sections 4.3–4.4. The implementation's density and DFT normalization are explicitly
derived above, rather than inferred from a visual resemblance or fitted scalar.
