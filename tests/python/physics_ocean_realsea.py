# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Physical RealSea regression checks against independent variance and dispersion expectations.

Run with: blender -b --factory-startup --python-exit-code 1 --python physics_ocean_realsea.py
The tests sample an actual evaluated dense modifier, not a copied spectrum implementation or
MATLAB-exported values. Duplicate periodic mesh endpoints are excluded from every statistic.
"""

import json
import math

import bpy
import numpy as np

G = 9.81


def make_ocean():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.object
    modifier = obj.modifiers.new("RealSea", "OCEAN")
    modifier.geometry_mode = "GENERATE"
    modifier.use_camera_lod = False
    modifier.spectrum = "REALSEA_PM"
    modifier.resolution = modifier.viewport_resolution = 12
    modifier.spatial_size = 128
    modifier.size = 1.0
    modifier.repeat_x = modifier.repeat_y = 1
    modifier.wind_velocity = 4.0
    modifier.depth = 1000.0
    modifier.choppiness = 0.0
    modifier.wave_alignment = 0.0
    modifier.wave_direction = 0.0
    modifier.wave_scale_min = 0.0
    modifier.use_wave_scale = False
    modifier.wave_scale = 1.0
    modifier.realsea_fmin = 0.2
    modifier.realsea_fmax = 0.6
    modifier.realsea_use_spread = False
    modifier.realsea_spread = 1.0
    modifier.fetch_jonswap = 300000.0
    modifier.sharpen_peak_jonswap = 0.33
    modifier.random_seed = 0
    modifier.time = 0.0
    return obj, modifier


def height_field(obj, modifier, **settings):
    for name, value in settings.items():
        setattr(modifier, name, value)
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        n = modifier.viewport_resolution ** 2
        assert len(mesh.vertices) == (n + 1) ** 2, len(mesh.vertices)
        coordinates = np.empty(3 * len(mesh.vertices), dtype=np.float32)
        mesh.vertices.foreach_get("co", coordinates)
        assert np.isfinite(coordinates).all()
        return coordinates.reshape(n + 1, n + 1, 3)[:-1, :-1, 2].astype(np.float64)
    finally:
        evaluated.to_mesh_clear()


def frequencies(modifier):
    n = modifier.viewport_resolution ** 2
    k_axis = np.fft.fftfreq(n, d=modifier.spatial_size / n) * (2.0 * np.pi)
    k = np.hypot(k_axis[:, None], k_axis[None, :])
    # The depth chosen in these tests makes the deep-water limit exact to floating precision.
    omega = np.sqrt(G * k)
    return k, omega, omega / (2.0 * np.pi)


def pm_density(modifier):
    """Independent deep-water PM density on the DFT grid, with isotropic angular density."""
    k, omega, f = frequencies(modifier)
    fp = 0.8772 * G / (2.0 * np.pi * modifier.wind_velocity)
    density = np.zeros_like(k)
    active = (f >= modifier.realsea_fmin) & (f <= modifier.realsea_fmax) & (k > 0)
    f_active = f[active]
    sf = (0.0081 * G ** 2 / ((2.0 * np.pi) ** 4 * f_active ** 5) *
          np.exp(-1.25 * (fp / f_active) ** 4))
    df_dk = G / (4.0 * np.pi * omega[active])
    density[active] = sf * df_dk / k[active] / (2.0 * np.pi)
    n = modifier.viewport_resolution ** 2
    if n % 2 == 0:
        density[n // 2, :] = density[:, n // 2] = 0.0
    return density


def jonswap_density(modifier, gamma):
    """Independent RealSea constant-alpha JONSWAP variance density in the deep-water limit."""
    k, omega, f = frequencies(modifier)
    wind_at_10_m = modifier.wind_velocity / 1.025
    peak_omega = 22.0 * (G ** 2 / (wind_at_10_m * modifier.fetch_jonswap)) ** (1.0 / 3.0)
    peak_frequency = peak_omega / (2.0 * np.pi)
    density = np.zeros_like(k)
    active = (f >= modifier.realsea_fmin) & (f <= modifier.realsea_fmax) & (k > 0)
    f_active = f[active]
    sigma = np.where(f_active <= peak_frequency, 0.07, 0.09)
    peak_shape = np.exp(-0.5 * ((f_active / peak_frequency - 1.0) / sigma) ** 2)
    sf = (0.0081 * G ** 2 / ((2.0 * np.pi) ** 4 * f_active ** 5) *
          np.exp(-1.25 * (peak_frequency / f_active) ** 4) * gamma ** peak_shape)
    df_dk = G / (4.0 * np.pi * omega[active])
    density[active] = sf * df_dk / k[active] / (2.0 * np.pi)
    n = modifier.viewport_resolution ** 2
    if n % 2 == 0:
        density[n // 2, :] = density[:, n // 2] = 0.0
    return density


def mean_variance(obj, modifier, seeds=12, **settings):
    values = [float(np.var(height_field(obj, modifier, random_seed=seed, **settings)))
              for seed in range(seeds)]
    return float(np.mean(values))


def test_contract_and_gain(obj, modifier):
    assert modifier.spectrum_implementation_version == 2
    assert modifier.bl_rna.properties["spectrum_implementation_version"].is_readonly
    baseline = height_field(obj, modifier)
    ignored_gain = height_field(obj, modifier, wave_scale=3.0)
    np.testing.assert_array_equal(baseline, ignored_gain)
    applied_gain = height_field(obj, modifier, use_wave_scale=True, wave_scale=2.0)
    np.testing.assert_allclose(applied_gain, baseline * 2.0, rtol=2e-6, atol=2e-7)
    height_field(obj, modifier, use_wave_scale=False, wave_scale=1.0)
    for spectrum in ("PHILLIPS", "PIERSON_MOSKOWITZ", "JONSWAP", "TEXEL_MARSEN_ARSLOE"):
        modifier.spectrum = spectrum
        assert modifier.spectrum_implementation_version == 1
    modifier.spectrum = "REALSEA_PM"


def test_mode_variance_and_evolution(obj, modifier):
    modifier.realsea_use_spread = False
    modifier.spectrum = "REALSEA_PM"
    n = modifier.viewport_resolution ** 2
    k, omega, f = frequencies(modifier)
    first_time, last_time = 0.137, 0.413
    expected = pm_density(modifier) * (2.0 * np.pi / modifier.spatial_size) ** 2
    active = expected > 0
    measured_mode_energy = np.zeros_like(expected)
    for seed in range(12):
        c0 = np.fft.fft2(height_field(obj, modifier, time=0.0, random_seed=seed)) / (n * n)
        c1 = np.fft.fft2(height_field(obj, modifier, time=first_time)) / (n * n)
        c2 = np.fft.fft2(height_field(obj, modifier, time=last_time)) / (n * n)
        positive = np.zeros_like(c0)
        positive[active] = ((c1[active] - c0[active] * np.exp(-1j * omega[active] * first_time)) /
                            (2j * np.sin(omega[active] * first_time)))
        negative = c0 - positive
        predicted = (positive * np.exp(1j * omega * last_time) +
                     negative * np.exp(-1j * omega * last_time))
        # Three real snapshots must follow the same correctly indexed modal dispersion.
        np.testing.assert_allclose(c2, predicted, rtol=2e-4, atol=2e-7)
        measured_mode_energy += np.abs(positive) ** 2
        assert np.max(np.abs(c0[~active])) < 2e-7
        if n % 2 == 0:
            assert np.max(np.abs(c0[n // 2, :])) < 2e-7
            assert np.max(np.abs(c0[:, n // 2])) < 2e-7
        opposite = np.conj(negative[np.ix_((-np.arange(n)) % n, (-np.arange(n)) % n)])
        np.testing.assert_allclose(positive, opposite, rtol=2e-4, atol=2e-7)
    observed = float(np.sum(measured_mode_energy) / 12)
    target = float(np.sum(expected) * 0.5)
    assert math.isclose(observed, target, rel_tol=0.04), (observed, target)
    field_variance = mean_variance(obj, modifier, time=0.2)
    assert math.isclose(field_variance, float(np.sum(expected)), rel_tol=0.04), (
        field_variance, float(np.sum(expected)))
    print(json.dumps({"check": "mode_variance", "observed_h0_variance": observed,
                      "expected_h0_variance": target, "field_variance": field_variance}))


def test_extent_and_resolution(obj, modifier):
    reports = []
    for extent, resolution in ((128, 12), (288, 18), (512, 24), (128, 16)):
        modifier.spatial_size = extent
        modifier.resolution = modifier.viewport_resolution = resolution
        expected = float(np.sum(pm_density(modifier)) * (2.0 * np.pi / extent) ** 2)
        observed = mean_variance(obj, modifier, time=0.2)
        assert math.isclose(observed, expected, rel_tol=0.06), (extent, resolution, observed, expected)
        reports.append({"extent": extent, "resolution": resolution,
                        "variance": observed, "expected_variance": expected})
    target = reports[0]["expected_variance"]
    assert all(math.isclose(report["variance"], target, rel_tol=0.08) for report in reports)
    print(json.dumps({"check": "extent_and_resolution", "cases": reports}))
    modifier.spatial_size = 128
    modifier.resolution = modifier.viewport_resolution = 12


def test_direction_and_spreading(obj, modifier):
    expected = float(np.sum(pm_density(modifier)) * (2.0 * np.pi / modifier.spatial_size) ** 2)
    reports = []
    for spread, angle in ((False, 0.0), (True, 0.0), (True, np.pi / 2), (True, np.pi / 4)):
        observed = mean_variance(obj, modifier, seeds=20, time=0.2,
                                 realsea_use_spread=spread, wave_direction=angle)
        assert math.isclose(observed, expected, rel_tol=0.10), (spread, angle, observed, expected)
        reports.append({"spread": spread, "direction": angle, "variance": observed})
    print(json.dumps({"check": "directional_energy", "cases": reports,
                      "expected_variance": expected}))
    modifier.realsea_use_spread = False
    modifier.wave_direction = 0.0


def test_jonswap_absolute_variance(obj, modifier):
    modifier.spectrum = "REALSEA_JONSWAP"
    modifier.wind_velocity = 4.0
    modifier.fetch_jonswap = 30000.0
    modifier.realsea_fmin = 0.2
    modifier.realsea_fmax = 0.6
    modifier.realsea_use_spread = False
    modifier.spatial_size = 128
    modifier.resolution = modifier.viewport_resolution = 12
    reports = []
    for gamma in (1.0, 3.3, 6.0):
        expected = float(np.sum(jonswap_density(modifier, gamma)) *
                         (2.0 * np.pi / modifier.spatial_size) ** 2)
        observed = mean_variance(obj, modifier, seeds=12, time=0.2,
                                 sharpen_peak_jonswap=gamma / 10.0)
        # Twelve realizations cover thousands of resolved modes in this band. The tolerance
        # allows finite Gaussian sampling variance, not a fitted change to the density scale.
        assert math.isclose(observed, expected, rel_tol=0.06), (gamma, observed, expected)
        reports.append({"gamma": gamma, "variance": observed, "expected_variance": expected})
    print(json.dumps({"check": "jonswap_absolute_variance", "cases": reports}))


def test_effective_controls(obj, modifier):
    modifier.spectrum = "REALSEA_JONSWAP"
    modifier.realsea_fmin = 0.05
    modifier.fetch_jonswap = 300000
    baseline = height_field(obj, modifier, sharpen_peak_jonswap=0.33)
    default_gamma = height_field(obj, modifier, sharpen_peak_jonswap=0.0)
    np.testing.assert_allclose(default_gamma, baseline, rtol=2e-6, atol=2e-7)
    gamma_one = height_field(obj, modifier, sharpen_peak_jonswap=0.1)
    gamma_six = height_field(obj, modifier, sharpen_peak_jonswap=0.6)
    assert np.var(gamma_six) > np.var(gamma_one)
    assert not np.array_equal(gamma_one, gamma_six)
    short_fetch = height_field(obj, modifier, fetch_jonswap=500)
    long_fetch = height_field(obj, modifier, fetch_jonswap=500000)
    assert not np.array_equal(short_fetch, long_fetch)
    # The slider's usual 0.1–4 range must not silently clamp explicitly supplied widths.
    for width in (0.05, 8.0):
        height_field(obj, modifier, realsea_use_spread=True, realsea_spread=width)
        assert math.isclose(modifier.realsea_spread, width, rel_tol=1e-6)
    # Reject reversed and equal positive bounds rather than changing the requested band.
    for lower, upper in ((0.8, 0.2), (0.2, 0.2)):
        modifier.realsea_fmin = lower
        modifier.realsea_fmax = upper
        bpy.context.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        assert len(evaluated.data.vertices) == len(obj.data.vertices)
    # Both cutoffs may be explicitly disabled with zero.
    height_field(obj, modifier, realsea_fmin=0.0, realsea_fmax=0.0)


def main():
    obj, modifier = make_ocean()
    test_contract_and_gain(obj, modifier)
    test_mode_variance_and_evolution(obj, modifier)
    test_extent_and_resolution(obj, modifier)
    test_direction_and_spreading(obj, modifier)
    test_jonswap_absolute_variance(obj, modifier)
    test_effective_controls(obj, modifier)
    print("REALSEA_PHYSICS_V2_PASS")


if __name__ == "__main__":
    main()
