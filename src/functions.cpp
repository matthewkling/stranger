#include <RcppArmadillo.h>
using namespace Rcpp;


// DEMOGRAPHY //////////////////////////////////////////////////////////////////

arma::imat rbinom_trans(arma::imat n,
                        arma::mat p,
                        std::default_random_engine gen) {
  arma::imat y(n.n_rows, n.n_cols);
  for(arma::uword i = 0; i < n.n_elem; ++i) {
    std::binomial_distribution<> d(n(i), p(i));
    y(i) = d(gen);
  }
  return y;
}


arma::icube rmultinom_trans(arma::mat pop,
                            arma::cube probs,
                            std::default_random_engine gen) {

  arma::imat popn = arma::conv_to<arma::imat>::from(pop);
  arma::icube y(size(probs), arma::fill::zeros);
  arma::mat p(size(pop), arma::fill::zeros);
  arma::mat m = 1 - sum(probs, 2); // mortality
  arma::imat u = popn; // unallocated

  for(arma::uword i = 0; i < probs.n_slices; ++i) {
    p = sum(probs.slices(i, probs.n_slices - 1), 2);
    y.slice(i) = arma::min(rbinom_trans(u, probs.slice(i) / (p + m), gen), u);
    u = u - y.slice(i);
  }

  return(y);
}



//' Perform a stage-based demographic transition
//'
//' @param N A 3-D array of population numbers for each life stage, over a spatial grid (x, y, class).
//' @param E A 3-D array of environmental data (x, y, variable).
//' @param alpha A matrix of transition intercepts (to, from).
//' @param beta A 3-D array of density dependence effects (to, from, modifier).
//' @param gamma A 3-D array of environmental effects (to, from, variable).
//' @param rand Randomize transitions instead of using matrix multiplication? (Boolean, default = TRUE).
//' @param seed Integer to seed random number generator.
//' @return A 3-D array of population numbers for each life stage.
//' @export
// [[Rcpp::export]]
arma::cube transition(arma::cube N,
                      arma::cube E,
                      arma::mat alpha,
                      arma::cube beta,
                      arma::cube gamma,
                      bool rand = true,
                      int seed = 1) {

  arma::cube NN(size(N), arma::fill::zeros);
  arma::cube p(size(N), arma::fill::zeros);
  double m = 0;

  std::default_random_engine gen(seed); // initialize random number generator

  for(arma::uword s = 0; s < alpha.n_cols; ++s) { // source class

    p.fill(0);

    // construct transition probabilities
    for(arma::uword t = 0; t < alpha.n_rows; ++t) { // target class

      if (alpha(t, s) +
          accu(beta.tube(t, s)) +
          accu(gamma.tube(t, s)) == 0) {
        continue;
      }

      // intercept
      p.slice(t).fill(alpha(t, s));

      // density dependence
      for(arma::uword d = 0; d < N.n_slices; ++d){
        m = beta(t, s, d);
        if (m != 0) {
          p.slice(t) = p.slice(t) + arma::conv_to<arma::mat>::from(N.slice(d)) * m;
        }
      }

      // environmental dependence
      for(arma::uword e = 0; e < E.n_slices; ++e){
        m = gamma(t, s, e);
        if (m != 0) {
          p.slice(t) = p.slice(t) + E.slice(e) * m;
        }
      }

    }

    // constrain individual and joint probabilities
    p = clamp(p, 0, 1);
    arma::mat psum = sum(p, 2);
    for(arma::uword x = 0; x < psum.n_rows; ++x) {
      for(arma::uword y = 0; y < psum.n_cols; ++y) {
        if(psum(x, y) > 1) {
          p.tube(x, y) = p.tube(x, y) / psum(x, y);
        }
      }
    }

    // perform class transition
    if (rand) {
      NN = NN + rmultinom_trans(N.slice(s), p, gen);
      ++seed;
    } else {
      for(arma::uword t = 0; t < alpha.n_rows; ++t) {
        NN.slice(t) = NN.slice(t) + N.slice(s) % p.slice(t);
      }
    }

  }

  return NN;
}


//' Reproduction across a spatial grid
//'
//' @param N A 3-D array of population numbers for each life stage, over a spatial grid (x, y, class).
//' @param f Integer vector of fecundity with a value for each class in \code{N}.
//' @return A matrix.
//' @export
// [[Rcpp::export]]
arma::mat reproduce(arma::cube N,
                    arma::vec f) {
  arma::mat y(N.n_rows, N.n_cols, arma::fill::zeros);

  for(arma::uword i = 0; i < N.n_slices; ++i){
    if (f(i) == 0) {
      continue;
    }
    y = y + N.slice(i) * f(i);
  }

  return(y);
}


// DISPERSAL ///////////////////////////////////////////////////////////////////


int rbinom_disp(int n,
                double p,
                std::default_random_engine gen) {
  std::binomial_distribution<> d(n, p);
  return d(gen);
}


arma::imat rmultinom_disp(int seeds,
                          arma::mat probs,
                          arma::uvec ind,
                          std::default_random_engine gen) {

  arma::imat y(size(probs), arma::fill::zeros); // post-dispersal counts
  double p = 1; // unallocated probability
  int u = seeds; // unallocated seeds
  arma::uword ii = 0;

  for(arma::uword i = 0; i < probs.n_elem; ++i) {
    ii = ind(i);
    p = accu(probs);
    y(ii) = std::min(rbinom_disp(u, probs(ii) / p, gen), u);
    u = u - y(ii);
    if (u == 0) {
      break;
    }
    probs(ii) = 0;
  }

  return(y);
}


//' Simulate dispersal across a spatial grid
//'
//' @param S A matrix of seed counts across a spatial grid.
//' @param N A neighbor matrix, e.g. produced by \code{neighborhood()}.
//' @param reflect Should dispersers exit the domain (\code{FALSE}) or bounce off the domain boundary (\code{TRUE}, default)?
//' @param rand Randomize dispersal? (default = \code{TRUE})
//' @param seed Integer to seed random number generator.
//' @return A matrix of post-dispersal seed counts of the same dimension as \code{S}.
//' @export
// [[Rcpp::export]]
arma::mat disperse(arma::mat S,
                   arma::mat N,
                   bool reflect = true,
                   bool rand = true,
                   int seed = 1) {

  std::default_random_engine gen(seed); // initialize random number generator

  arma::uvec Ni = arma::sort_index(N, "descent"); // order to evaluate neighbors

  int r = (N.n_rows - 1) / 2; // window radius
  arma::mat T(S.n_rows + r * 2, S.n_cols + r * 2, arma::fill::zeros); // padded grid

  for(arma::uword a = 0; a < S.n_rows; ++a) {
    for(arma::uword b = 0; b < S.n_cols; ++b) {

      if (rand) {
        T.submat(a, b, a + r * 2, b + r * 2) =
          T.submat(a, b, a + r * 2, b + r * 2) +
          rmultinom_disp(S(a, b), N, Ni, gen);
      } else {
        T.submat(a, b, a + r * 2, b + r * 2) =
          T.submat(a, b, a + r * 2, b + r * 2) +
          S(a, b) * N;
      }

    }
  }

  if (reflect) {
    for(int i = 0; i < r; ++i){
      T.row(r * 2 - 1 - i) = T.row(r * 2 - 1 - i) + T.row(i);
      T.col(r * 2 - 1 - i) = T.col(r * 2 - 1 - i) + T.col(i);
      T.row(T.n_rows - (r * 2 - 1 - i) - 1) =
        T.row(T.n_rows - (r * 2 - 1 - i) - 1) + T.row(T.n_rows - 1 - i);
      T.col(T.n_cols - (r * 2 - 1 - i) - 1) =
        T.col(T.n_cols - (r * 2 - 1 - i) - 1) + T.col(T.n_cols - 1 - i);
    }
  }

  return T.submat(r, r, S.n_rows + r - 1, S.n_cols + r - 1);
}



// SIMULATION //////////////////////////////////////////////////////////////////

//' Run a range simulation
//'
//' @param N A 3-D array of population numbers for each life stage, over a spatial grid (x, y, class).
//' @param env A list of environmental data. Each element list element should be a 3-D array of dimensions (x, y, variable).
//' The list should have one element for time-invariant environment, or \code{nsteps} elements for time-varying environment.
//' @param alpha, \code{beta, gamma, fecundity} Demographic parameters; see \code{?transition} and \code{?reproduce}.
//' @param nb Neighborhood matrix; e.g. output from \code{neighborhood}.
//' @param rand Randomize transitions instead of using matrix multiplication? (Boolean, default = TRUE).
//' @param seed Integer to seed random number generator.
//' @return A 3-D array of population numbers for each life stage.
//' @export
// [[Rcpp::export]]
arma::cube sim(arma::cube N,
               arma::field<arma::cube> env,
               arma::mat alpha,
               arma::cube beta,
               arma::cube gamma,
               arma::vec fecundity,
               arma::mat nb,
               bool reflect = true,
               bool rand = true,
               int seed = 1,
               int record = 0,
               arma::uword nsteps = 100) {

  arma::vec ei(nsteps + 1, arma::fill::zeros);
  if (env.n_elem > 1) {
    ei = arma::linspace(0, nsteps, nsteps + 1);
  }

  arma::cube d(N.n_rows, N.n_cols, nsteps + 1, arma::fill::zeros);
  d.slice(0) = N.slice(record);

  for(arma::uword i = 0; i < nsteps; ++i){
    N = transition(N, env(ei(i)), alpha, beta, gamma, rand, seed * i);
    N.slice(0) = N.slice(0) + disperse(reproduce(N, fecundity), nb, reflect, rand, seed * i + 1);
    d.slice(i + 1) = N.slice(record);
  }

  return(d);
}
