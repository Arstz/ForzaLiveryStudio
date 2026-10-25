"""Native-grid rolling guidance using a color guided filter."""

import cv2
import numpy as np

DEFAULT_GUIDANCE_SCALE = 4.


def rolling_guidance(rgb, domain, scale=DEFAULT_GUIDANCE_SCALE, range_sigma=.075, iterations=3):
    if scale == 0 or iterations == 0:
        return rgb.copy()
    source = rgb.astype(np.float32) / 255
    weights = domain.astype(np.float32)
    blurred_weight = cv2.GaussianBlur(weights, (0, 0), scale)
    guide = cv2.GaussianBlur(source * weights[:, :, None], (0, 0), scale) \
        / np.maximum(blurred_weight[:, :, None], 1e-6)
    radius = max(1, int(np.ceil(2 * scale)))
    window = (2 * radius + 1, 2 * radius + 1)
    mass = cv2.boxFilter(weights, -1, window)

    def mean(value):
        if value.ndim == 2:
            return cv2.boxFilter(value * weights, -1, window) / np.maximum(mass, 1e-6)
        return cv2.boxFilter(value * weights[:, :, None], -1, window) / np.maximum(mass[:, :, None], 1e-6)

    mean_source = mean(source)
    for _ in range(iterations):
        mean_guide = mean(guide)
        product = (guide[:, :, :, None] * guide[:, :, None, :]).reshape(*domain.shape, 9)
        covariance = mean(product).reshape(*domain.shape, 3, 3) \
            - mean_guide[:, :, :, None] * mean_guide[:, :, None, :]
        cross = mean((guide[:, :, :, None] * source[:, :, None, :]).reshape(*domain.shape, 9)) \
            .reshape(*domain.shape, 3, 3) - mean_guide[:, :, :, None] * mean_source[:, :, None, :]
        covariance += np.eye(3, dtype=np.float32) * range_sigma ** 2
        coefficients = np.linalg.solve(covariance, cross)
        intercept = mean_source - np.einsum('...ij,...i->...j', coefficients, mean_guide)
        averaged = mean(coefficients.reshape(*domain.shape, 9)).reshape(*domain.shape, 3, 3)
        guide = np.einsum('...ij,...i->...j', averaged, guide) + mean(intercept)
        guide = np.clip(guide, 0, 1)
    if not np.isfinite(guide[domain]).all():
        raise RuntimeError('Structure guidance produced non-finite colors')
    return np.rint(guide * 255).astype(np.uint8)
