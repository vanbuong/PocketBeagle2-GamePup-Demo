# SPDX-License-Identifier: GPL-2.0-only
"""LQR design of the balance gains from the physical parameters (doc 05, 5.1 and 5.10).

Prints the cascade gains (Kp, Kd, Kvp, Kvi, kv_ff) matching core/src/balance.c for a given parameter set:
    u = x_gain*(x - x_ref) + v_gain*(v - v_ref) + Kp*theta + Kd*omega + kv_ff*v_ref
    Kvp = v_gain / Kp,  Kvi = x_gain / Kp,  kv_ff = Ke / r
Usage: python3 lqr_design.py
"""
import numpy as np
import scipy.linalg as la

from plant import G, Params


def linear_model(P: Params):
    M = P.m_eff
    m, l, I, r = P.m, P.l, P.I, P.r
    D = (M + m) * (I + m * l * l) - (m * l) ** 2
    b = 2 * P.Kt / (P.R * r)
    c = 2 * P.Kt * P.Ke / (P.R * r ** 2)
    A = np.array([[0, 1, 0, 0],
                  [0, -(I + m * l * l) * c / D, -m * m * G * l * l / D, 0],
                  [0, 0, 0, 1],
                  [0, m * l * c / D, m * G * l * (M + m) / D, 0]])
    B = np.array([[0], [(I + m * l * l) * b / D], [0], [-m * l * b / D]])
    return A, B


def design(P: Params, q=(1.0, 1.0, 100.0, 1.0), rho=1.0):
    A, B = linear_model(P)
    Q = np.diag(q)
    R = np.array([[rho]])
    X = la.solve_continuous_are(A, B, Q, R)
    K = (np.linalg.solve(R, B.T @ X))[0]  # u = -K s
    x_gain, v_gain, kp, kd = -K[0], -K[1], -K[2], -K[3]
    return {"Kp": kp, "Kd": kd, "Kvp": v_gain / kp, "Kvi": x_gain / kp, "kv_ff": P.Ke / P.r,
            "poles_open": np.linalg.eigvals(A), "poles_closed": np.linalg.eigvals(A - B @ K.reshape(1, 4))}


if __name__ == "__main__":
    d = design(Params())
    for k in ("Kp", "Kd", "Kvp", "Kvi", "kv_ff"):
        print(f"{k:6s} {d[k]:8.3f}")
    print("open-loop poles  ", np.round(d["poles_open"], 3))
    print("closed-loop poles", np.round(d["poles_closed"], 3))
