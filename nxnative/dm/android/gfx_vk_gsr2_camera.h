// The guest stores a row-major inverse view-projection matrix; SGSR2 consumes
// four columns of previousVP * currentInverseVP. Use doubles for world coordinates.
#pragma once
#include <math.h>
#include <string.h>
static inline int gsr2_reprojection(const float previous_inverse[16], const float current_inverse[16], float columns[16]) {
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            m[r][c] = previous_inverse[4 * r + c];
            m[r][4 + c] = current_inverse[4 * r + c];
            if (!isfinite(m[r][c]) || !isfinite(m[r][4 + c])) return 0;
        }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(m[r][c]) > fabs(m[pivot][c])) pivot = r;
        if (fabs(m[pivot][c]) < 1e-10) return 0;
        for (int k = 0; k < 8; k++) {
            double tmp = m[c][k];
            m[c][k] = m[pivot][k];
            m[pivot][k] = tmp;
        }
        double d = m[c][c];
        for (int k = c; k < 8; k++)
            m[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = m[r][c];
                for (int k = c; k < 8; k++)
                    m[r][k] -= f * m[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            columns[4 * c + r] = (float)m[r][4 + c];
            if (!isfinite(columns[4 * c + r])) return 0;
        }
    return 1;
}
