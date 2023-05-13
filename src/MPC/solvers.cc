/**
 * @file solvers.cc
 * @author Geir Ola Tvinnereim
 * @brief 
 * @version 0.1
 * @date 2023-04-19
 * 
 * @copyright Released under the terms of the BSD 3-Clause License
 * 
 */
#include "MPC/solvers.h"
#include "MPC/condensed_qp.h"
#include <OsqpEigen/OsqpEigen.h>

#include <stdexcept>
#include <iostream>
using SparseXd = Eigen::SparseMatrix<double>; 

void SRSolver(int T, MatrixXd& u_mat, MatrixXd& y_pred, FSRModel& fsr, const MPCConfig& conf, const VectorXd& z_min, 
             const VectorXd& z_max, const MatrixXd& ref) {
    // Initialize solver:
    OsqpEigen::Solver solver;
    solver.settings()->setWarmStart(true); // Starts primal and dual variables from previous QP
    solver.settings()->setVerbosity(false); // Disable printing

    // MPC Scenario variables:
    int P = fsr.getP(), M = fsr.getM(), W = fsr.getW(), n_MV = fsr.getN_MV(), n_CV = fsr.getN_CV(); 
    // Define QP sizes:
    const int n = M * n_MV + 2 * n_CV; // #Optimization variables, dim(z_cd)
    const int m = 2 * M * n_MV + 2 * (P-W) * n_CV + 2 * n_CV; // #Constraints, dim(z_st)
    const int a = M * n_MV; // dim(du)
    const VectorXd z_max_pop = PopulateConstraints(z_max, conf, a, n_MV, n_CV), z_min_pop = PopulateConstraints(z_min, conf, a, n_MV, n_CV);
    // z_min_max_pop are respectively lower and upper populated constraints

    solver.data()->setNumberOfVariables(n);
    solver.data()->setNumberOfConstraints(m);

    // Define Cost function variables: 
    SparseXd Q_bar, R_bar, Gamma = setGamma(M, n_MV);
    MatrixXd K_inv = setKInv(a), theta = fsr.getTheta();
    // Dynamic variables:
    VectorXd q, l = VectorXd::Zero(m), u = VectorXd::Zero(m); // l and u are lower and upper constraints, z_cd 
    VectorXd c_l = ConfigureConstraint(z_min_pop, m, a, false), c_u = ConfigureConstraint(z_max_pop, m, a, true);

    // NB! W-dependant
    setWeightMatrices(Q_bar, R_bar, conf);
    SparseXd G = setHessianMatrix(Q_bar, R_bar, theta, a, n);
    setGradientVector(q, fsr, Q_bar, ref, conf, n, 0); // Initial gradient
    SparseXd A = setConstraintMatrix(theta, m, n, a, n_CV);
    setConstraintVectors(l, u, fsr, c_l, c_u, K_inv, Gamma, m, a);

    if (!solver.data()->setHessianMatrix(G)) { throw std::runtime_error("Cannot initialize Hessian"); }
    if (!solver.data()->setGradient(q)) { throw std::runtime_error("Cannot initialize Gradient"); }
    if (!solver.data()->setLinearConstraintsMatrix(A)) { throw std::runtime_error("Cannot initialize constraint matrix"); }
    if (!solver.data()->setLowerBound(l)) { throw std::runtime_error("Cannot initialize lower bound"); }
    if (!solver.data()->setUpperBound(u)) { throw std::runtime_error("Cannot initialize upper bound"); }
    if (!solver.initSolver()) { throw std::runtime_error("Cannot initialize solver"); }

    u_mat = MatrixXd::Zero(n_MV, T);
    y_pred = MatrixXd::Zero(n_CV, T + P);
    SparseXd omega_u = setOmegaU(M, n_MV);

    // MPC loop:
    for (int k = 0; k <= T; k++) { // Simulate one step more to get predictions.
         // Optimize:
         if (solver.solveProblem() != OsqpEigen::ErrorExitFlag::NoError) { throw std::runtime_error("Cannot solve problem"); }

         // Claim solution:
         VectorXd z_st = solver.getSolution(); // [dU, eta_h, eta_l]
         VectorXd z = z_st(Eigen::seq(0, a - 1)); // [dU]
         VectorXd du = omega_u * z; // MPC actuation

        // Store optimal du and y_pref: Before update!
        if (k == T) {           
            y_pred.block(0, k, n_CV, P) = fsr.getY(z, true);
        } else {
            // Propagate FSR model:
            fsr.UpdateU(du);

            u_mat.col(k) = fsr.getUK();
            y_pred.col(k) = fsr.getY(z);

            // Update MPC problem:
            setConstraintVectors(l, u, fsr, c_l, c_u, K_inv, Gamma, m, a);
            setGradientVector(q, fsr, Q_bar, ref, conf, n, k); 

            // Check if bounds are valid:
            if (!solver.updateBounds(l, u)) { throw std::runtime_error("Cannot update bounds"); }
            if (!solver.updateGradient(q)) { throw std::runtime_error("Cannot update gradient"); }
        }
    }
}

void SRSolver(int T, MatrixXd& u_mat, MatrixXd& y_pred, FSRModel& fsr_sim, FSRModel& fsr_cost, const MPCConfig& conf, const VectorXd& z_min, 
             const VectorXd& z_max, const MatrixXd& ref) {
    // Initialize solver:
    OsqpEigen::Solver solver;
    solver.settings()->setWarmStart(true); // Starts primal and dual variables from previous QP
    solver.settings()->setVerbosity(false); // Disable printing

    // MPC Scenario variables:
    int P = fsr_cost.getP(), M = fsr_cost.getM(), W = fsr_cost.getW(), n_MV = fsr_cost.getN_MV(), n_CV = fsr_cost.getN_CV(); 
    // Define QP sizes:
    const int n = M * n_MV + 2 * n_CV; // #Optimization variables, dim(z_cd)
    const int m = 2 * M * n_MV + 2 * (P-W) * n_CV + 2 * n_CV; // #Constraints, dim(z_st)
    const int a = M * n_MV; // dim(du)
    const VectorXd z_max_pop = PopulateConstraints(z_max, conf, a, n_MV, n_CV), z_min_pop = PopulateConstraints(z_min, conf, a, n_MV, n_CV);
    // z_min_max_pop are respectively lower and upper populated constraints

    solver.data()->setNumberOfVariables(n);
    solver.data()->setNumberOfConstraints(m);

    // Define Cost function variables: 
    SparseXd Q_bar, R_bar, Gamma = setGamma(M, n_MV);
    MatrixXd K_inv = setKInv(a), theta = fsr_cost.getTheta();
    // Dynamic variables:
    VectorXd q, l = VectorXd::Zero(m), u = VectorXd::Zero(m); // l and u are lower and upper constraints, z_cd 
    VectorXd c_l = ConfigureConstraint(z_min_pop, m, a, false), c_u = ConfigureConstraint(z_max_pop, m, a, true);

    // NB! W-dependant
    setWeightMatrices(Q_bar, R_bar, conf);
    SparseXd G = setHessianMatrix(Q_bar, R_bar, theta, a, n);
    setGradientVector(q, fsr_cost, Q_bar, ref, conf, n, 0); // Initial gradient
    SparseXd A = setConstraintMatrix(theta, m, n, a, n_CV);
    setConstraintVectors(l, u, fsr_cost, c_l, c_u, K_inv, Gamma, m, a);

    if (!solver.data()->setHessianMatrix(G)) { throw std::runtime_error("Cannot initialize Hessian"); }
    if (!solver.data()->setGradient(q)) { throw std::runtime_error("Cannot initialize Gradient"); }
    if (!solver.data()->setLinearConstraintsMatrix(A)) { throw std::runtime_error("Cannot initialize constraint matrix"); }
    if (!solver.data()->setLowerBound(l)) { throw std::runtime_error("Cannot initialize lower bound"); }
    if (!solver.data()->setUpperBound(u)) { throw std::runtime_error("Cannot initialize upper bound"); }
    if (!solver.initSolver()) { throw std::runtime_error("Cannot initialize solver"); }

    u_mat = MatrixXd::Zero(n_MV, T);
    y_pred = MatrixXd::Zero(n_CV, T + P);
    SparseXd omega_u = setOmegaU(M, n_MV);

    // MPC loop:
    for (int k = 0; k <= T; k++) { // Simulate one step more to get predictions.
        // Optimize:
        if (solver.solveProblem() != OsqpEigen::ErrorExitFlag::NoError) { throw std::runtime_error("Cannot solve problem"); }

        // Claim solution:
        VectorXd z_st = solver.getSolution(); // [dU, eta_h, eta_l]
        VectorXd z = z_st(Eigen::seq(0, a - 1)); // [dU]
        VectorXd du = omega_u * z; // MPC actuation

        // Store optimal du and y_pref: Before update!
        if (k == T) {           
            y_pred.block(0, k, n_CV, P) = fsr_sim.getY(z, true);
        } else {
            // Propagate FSR models: Update both! 
            fsr_sim.UpdateU(du);
            fsr_cost.UpdateU(du);

            u_mat.col(k) = fsr_sim.getUK();
            y_pred.col(k) = fsr_sim.getY(z);

            // Update MPC problem:
            setConstraintVectors(l, u, fsr_cost, c_l, c_u, K_inv, Gamma, m, a);
            setGradientVector(q, fsr_cost, Q_bar, ref, conf, n, k); 

            // Check if bounds are valid:
            if (!solver.updateBounds(l, u)) { throw std::runtime_error("Cannot update bounds"); }
            if (!solver.updateGradient(q)) { throw std::runtime_error("Cannot update gradient"); }
        }
    }
}