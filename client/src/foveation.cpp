#include "foveation.h"

#include <math.h>

void foveation_axis(FoveationAxis *a, uint32_t expanded, float center_size, float center_shift, float edge_ratio)
{
    double n = expanded, e = edge_ratio;
    a->expanded = expanded;
    a->e = e;
    if (!(e > 1.0) || !(center_size < 1.0f)) {
        a->identity = true;
        a->compressed = (expanded + 31) / 32 * 32;
        a->ratio = n / a->compressed;
        return;
    }
    a->identity = false;
    // The edges are aligned to whole blocks of 2 * edge_ratio pixels.
    double edge_size = n - center_size * n;
    double cs = 1.0 - ceil(edge_size / (e * 2.0)) * (e * 2.0) / n;
    double edge_aligned = n - cs * n;
    double shift = edge_aligned > 0 ? ceil(center_shift * edge_aligned / (e * 2.0)) * (e * 2.0) / edge_aligned : 0;
    double scale = cs + (1.0 - cs) / e;
    double optimized = scale * n;
    a->compressed = (uint32_t)(ceil(optimized / 32.0) * 32.0);
    a->ratio = optimized / a->compressed;

    double c0 = (1.0 - cs) * 0.5;
    a->c1 = (e - 1.0) * c0 * (shift + 1.0) / e;
    a->c2 = (e - 1.0) * cs + 1.0;
    a->lo = c0 * (shift + 1.0);
    a->hi = c0 * (shift - 1.0) + 1.0;
    double lo_c = c0 * (shift + 1.0) / a->c2;
    double hi_c = c0 * (shift - 1.0) / a->c2 + 1.0;
    a->a_l = a->b_l = a->a_r = a->b_r = a->c_r = 0;
    if (lo_c > 0) {
        a->a_l = a->c2 * (1.0 - e) / (e * lo_c);
        a->b_l = (a->c1 + a->c2 * lo_c) / lo_c;
    }
    if (hi_c < 1.0) {
        double r = 1.0 - hi_c;
        a->a_r = a->c2 * (e - 1.0) / (e * r);
        a->b_r = (a->c2 - e * a->c1 - 2.0 * e * a->c2 + a->c2 * e * r + e) / (e * r);
        a->c_r = (a->c2 * e - a->c2) * (a->c1 - hi_c + a->c2 * hi_c) / (e * r * r);
    }
}

double foveation_map(const FoveationAxis *a, double u)
{
    if (a->identity)
        return u * a->ratio;
    double v;
    if (u < a->lo && a->a_l != 0) {
        double d = a->b_l * a->b_l + 4.0 * a->a_l * u;
        v = (-a->b_l + sqrt(d > 0 ? d : 0)) / (2.0 * a->a_l);
    } else if (u > a->hi && a->a_r != 0) {
        double d = a->b_r * a->b_r - 4.0 * (a->c_r - a->a_r * u);
        v = (-a->b_r + sqrt(d > 0 ? d : 0)) / (2.0 * a->a_r);
    } else {
        v = (u - a->c1) * a->e / a->c2;
    }
    return v * a->ratio;
}
