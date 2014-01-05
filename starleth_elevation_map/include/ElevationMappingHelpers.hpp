/*
 * ElevationMapHelpers.hpp
 *
 *  Created on: Jan 4, 2014
 *      Author: Péter Fankhauser
 *	 Institute: ETH Zurich, Autonomous Systems Lab
 */

#pragma once

namespace starleth_elevation_map {

template<typename Scalar>
struct VarianceClampOperator
{
  VarianceClampOperator(const Scalar& minVariance, const Scalar& maxVariance)
      : minVariance_(minVariance),
        maxVariance_(maxVariance)
  {
  }
  const Scalar operator()(const Scalar& x) const
  {
    return x < minVariance_ ? minVariance_ : (x > maxVariance_ ? std::numeric_limits<double>::infinity() : x);
  }
  Scalar minVariance_, maxVariance_;
};

} /* namespace starleth_elevation_map */