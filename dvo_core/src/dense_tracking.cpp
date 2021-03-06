/**
 *  This file is part of dvo.
 *
 *  Copyright 2012 Christian Kerl <christian.kerl@in.tum.de> (Technical University of Munich)
 *  For more information see <http://vision.in.tum.de/data/software/dvo>.
 *
 *  dvo is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  dvo is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with dvo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iomanip>

#include <dvo/dense_tracking.h>
#include <dvo/dense_tracking_impl.h>

#include <assert.h>
#include <sophus/se3.hpp>

#include <Eigen/Core>

#include <dvo/core/datatypes.h>
#include <dvo/core/point_selection_predicates.h>
#include <dvo/util/revertable.h>
#include <dvo/util/stopwatch.h>
#include <dvo/util/id_generator.h>
#include <dvo/util/histogram.h>
#include <dvo/visualization/visualizer.h>

namespace dvo
{

using namespace dvo::core;
using namespace dvo::util;

static inline bool isfinite(const float& v)
{
  return std::isfinite(v);
}

const DenseTracker::Config& DenseTracker::getDefaultConfig()
{
  static Config defaultConfig;

  return defaultConfig;
}

static const Eigen::IOFormat YamlArrayFmt(Eigen::FullPrecision, Eigen::DontAlignCols, ",", ",", "", "", "[", "]");

DenseTracker::DenseTracker(const Config& config) :
    itctx_(cfg),
    weight_calculation_(),
    selection_predicate_(),
    reference_selection_(selection_predicate_)
{
  configure(config);
}

DenseTracker::DenseTracker(const DenseTracker& other) :
  itctx_(cfg),
  weight_calculation_(),
  selection_predicate_(),
  reference_selection_(selection_predicate_)
{
  configure(other.configuration());
}

void DenseTracker::configure(const Config& config)
{
  assert(config.IsSane());

  cfg = config;

  selection_predicate_.intensity_threshold = cfg.IntensityDerivativeThreshold;
  selection_predicate_.depth_threshold = cfg.DepthDerivativeThreshold;

  if(cfg.UseWeighting)
  {
    weight_calculation_
      .scaleEstimator(ScaleEstimators::get(cfg.ScaleEstimatorType))
      .scaleEstimator()->configure(cfg.ScaleEstimatorParam);

    weight_calculation_
      .influenceFunction(InfluenceFunctions::get(cfg.InfluenceFuntionType))
      .influenceFunction()->configure(cfg.InfluenceFunctionParam);
  }
  else
  {
    weight_calculation_
      .scaleEstimator(ScaleEstimators::get(ScaleEstimators::Unit))
      .influenceFunction(InfluenceFunctions::get(InfluenceFunctions::Unit));
  }
}

bool DenseTracker::match(RgbdImagePyramid& reference, RgbdImagePyramid& current, Eigen::Affine3d& transformation)
{
  Result result;
  result.Transformation = transformation;

  bool success = match(reference, current, result);

  transformation = result.Transformation;

  return success;
}

bool DenseTracker::match(dvo::core::PointSelection& reference, RgbdImagePyramid& current, Eigen::Affine3d& transformation)
{
  Result result;
  result.Transformation = transformation;

  bool success = match(reference, current, result);

  transformation = result.Transformation;

  return success;
}

bool DenseTracker::match(dvo::core::RgbdImagePyramid& reference, dvo::core::RgbdImagePyramid& current, dvo::DenseTracker::Result& result)
{
  reference.compute(cfg.getNumLevels());
  reference_selection_.setRgbdImagePyramid(reference);

  return match(reference_selection_, current, result);
}

bool DenseTracker::match(dvo::core::PointSelection& reference, dvo::core::RgbdImagePyramid& current, dvo::DenseTracker::Result& result)
{
  current.compute(cfg.getNumLevels());

  bool success = true;

  if(!cfg.UseInitialEstimate)
  {
    result.Transformation.setIdentity();
  }

  // our first increment is the given guess
  Sophus::SE3d inc(result.Transformation.rotation(), result.Transformation.translation());

  Revertable<Sophus::SE3d> initial(inc);
  Revertable<Sophus::SE3d> estimate;

  bool accept = true;

  //static stopwatch_collection sw_level(5, "l", 100);
  //static stopwatch_collection sw_it(5, "it@l", 500);
  //static stopwatch_collection sw_error(5, "err@l", 500);
  //static stopwatch_collection sw_linsys(5, "linsys@l", 500);
  //static stopwatch_collection sw_prep(5, "prep@l", 100);

  if(points_error.size() < reference.getMaximumNumberOfPoints(cfg.LastLevel))
    points_error.resize(reference.getMaximumNumberOfPoints(cfg.LastLevel));
  if(residuals.size() < reference.getMaximumNumberOfPoints(cfg.LastLevel))
    residuals.resize(reference.getMaximumNumberOfPoints(cfg.LastLevel));
  if(weights.size() < reference.getMaximumNumberOfPoints(cfg.LastLevel))
    weights.resize(reference.getMaximumNumberOfPoints(cfg.LastLevel));

  std::vector<uint8_t> valid_residuals;

  bool debug = false;
  if(debug)
  {
    reference.debug(true);
    valid_residuals.resize(reference.getMaximumNumberOfPoints(cfg.LastLevel));
  }
  /*
  std::stringstream name;
  name << std::setiosflags(std::ios::fixed) << std::setprecision(2) << current.timestamp() << "_error.avi";

  cv::Size s = reference.getRgbdImagePyramid().level(size_t(cfg.LastLevel)).intensity.size();
  cv::Mat video_frame(s.height, s.width * 2, CV_32FC1), video_frame_u8;
  cv::VideoWriter vw(name.str(), CV_FOURCC('P','I','M','1'), 30, video_frame.size(), false);
  float rgb_max = 0.0;
  float depth_max = 0.0;

  std::stringstream name1;
  name1 << std::setiosflags(std::ios::fixed) << std::setprecision(2) << current.timestamp() << "_ref.png";

  cv::imwrite(name1.str(), current.level(0).rgb);

  std::stringstream name2;
  name2 << std::setiosflags(std::ios::fixed) << std::setprecision(2) << current.timestamp() << "_cur.png";

  cv::imwrite(name2.str(), reference.getRgbdImagePyramid().level(0).rgb);
  */
  Eigen::Vector2f mean;
  mean.setZero();
  Eigen::Matrix2f /*first_precision,*/ precision;
  precision.setZero();

  for(itctx_.Level = cfg.FirstLevel; itctx_.Level >= cfg.LastLevel; --itctx_.Level)
  {
    result.Statistics.Levels.push_back(LevelStats());
    LevelStats& level_stats = result.Statistics.Levels.back();

    mean.setZero();
    precision.setZero();

    // reset error after every pyramid level? yes because errors from different levels are not comparable
    itctx_.Iteration = 0;
    itctx_.Error = std::numeric_limits<double>::max();

    RgbdImage& cur = current.level(itctx_.Level);
    const IntrinsicMatrix& K = cur.camera().intrinsics();

    Vector8f wcur, wref;
    // i z idx idy zdx zdy
    float wcur_id = 0.5f, wref_id = 0.5f, wcur_zd = 1.0f, wref_zd = 0.0f;

    wcur <<  1.0f / 255.0f,  1.0f, wcur_id * K.fx() / 255.0f, wcur_id * K.fy() / 255.0f, wcur_zd * K.fx(), wcur_zd * K.fy(), 0.0f, 0.0f;
    wref << -1.0f / 255.0f, -1.0f, wref_id * K.fx() / 255.0f, wref_id * K.fy() / 255.0f, wref_zd * K.fx(), wref_zd * K.fy(), 0.0f, 0.0f;

//    sw_prep[itctx_.Level].start();


    PointSelection::PointIterator first_point, last_point;
    reference.select(itctx_.Level, K, first_point, last_point);
    cur.buildAccelerationStructure();

    level_stats.Id = itctx_.Level;
    level_stats.MaxValidPixels = reference.getMaximumNumberOfPoints(itctx_.Level);
    level_stats.ValidPixels = last_point - first_point;

//    sw_prep[itctx_.Level].stopAndPrint();

    NormalEquationsLeastSquares ls;
    Matrix6d A;
    Vector6d x, b;
    x = inc.log();

    ComputeResidualsResult compute_residuals_result;
    compute_residuals_result.first_point_error = points_error.begin();
    compute_residuals_result.first_residual = residuals.begin();
    compute_residuals_result.first_valid_flag = valid_residuals.begin();


//    sw_level[itctx_.Level].start();
    do
    {
      level_stats.Iterations.push_back(IterationStats());
      IterationStats& iteration_stats = level_stats.Iterations.back();
      iteration_stats.Id = itctx_.Iteration;

//      sw_it[itctx_.Level].start();

      double total_error = 0.0f;
//      sw_error[itctx_.Level].start();
      Eigen::Affine3f transformf;

        inc = Sophus::SE3d::exp(x);
        initial.update() = inc.inverse() * initial();
        estimate.update() = inc * estimate();

        transformf = estimate().matrix().cast<float>();

        if(debug)
        {
          dvo::core::computeResidualsAndValidFlagsSse(first_point, last_point, cur, K, transformf, wref, wcur, compute_residuals_result);
        }
        else
        {
          dvo::core::computeResidualsSse(first_point, last_point, cur, K, transformf, wref, wcur, compute_residuals_result);
        }
        size_t n = (compute_residuals_result.last_residual - compute_residuals_result.first_residual);

        if(itctx_.IsFirstIterationOnLevel())
        {
          std::fill(weights.begin(), weights.begin() + n, 1.0f);
        }
        else
        {
          dvo::core::computeWeightsSse(compute_residuals_result.first_residual, compute_residuals_result.last_residual, weights.begin(), mean, precision);
        }

        precision = dvo::core::computeScaleSse(compute_residuals_result.first_residual, compute_residuals_result.last_residual, weights.begin(), mean).inverse();

        float ll = computeCompleteDataLogLikelihood(compute_residuals_result.first_residual, compute_residuals_result.last_residual, weights.begin(), mean, precision);

        iteration_stats.ValidConstraints = n;
        iteration_stats.TDistributionLogLikelihood = -ll;
        iteration_stats.TDistributionMean = mean.cast<double>();
        iteration_stats.TDistributionPrecision = precision.cast<double>();
        iteration_stats.PriorLogLikelihood = cfg.Mu * initial().log().squaredNorm();

        total_error = -ll;//iteration_stats.TDistributionLogLikelihood + iteration_stats.PriorLogLikelihood;
        //itctx_.Error = total_error;

        if(debug)
        {
          /*
          cv::Mat debug_idx, debug_img, debug_img_scaled, weight_img, weight_img_scaled, rgb_error_img, rgb_error_img_scaled, depth_error_img, depth_error_img_scaled;

          reference.getDebugIndex(itctx_.Level, debug_idx);

          debug_img = cv::Mat::zeros(debug_idx.size(), CV_8UC3);
          weight_img = cv::Mat::zeros(debug_idx.size(), CV_32FC1);
          rgb_error_img = cv::Mat::zeros(debug_idx.size(), CV_32FC1);
          depth_error_img = cv::Mat::zeros(debug_idx.size(), CV_32FC1);

          WeightIterator w_it = weights.begin();
          ResidualIterator r_it = residuals.begin();


          ResidualIterator r_it = compute_residuals_result.first_residual;
          ValidFlagIterator valid_it = compute_residuals_result.first_valid_flag;
          uint8_t *point_it = debug_idx.ptr<uint8_t>();
          float *weight_img_it = weight_img.ptr<float>();
          float *rgb_error_img_it = rgb_error_img.ptr<float>();
          float *depth_error_img_it = depth_error_img.ptr<float>();
          cv::Vec3b* img_it = debug_img.ptr<cv::Vec3b>();
          cv::Vec3b* img_end = debug_img.ptr<cv::Vec3b>() + debug_img.total();

          for(; img_it != img_end; ++img_it, ++weight_img_it, ++point_it, ++rgb_error_img_it, ++depth_error_img_it)
          {
            if(*point_it == 1)
            {
              img_it->val[2] = 255;

              if(*valid_it == 1)
              {
                img_it->val[1] = 255;

                *rgb_error_img_it = std::abs(r_it->coeff(0));
                *depth_error_img_it = std::abs(r_it->coeff(1));

                if(itctx_.IsFirstIteration())
                {
                  rgb_max = std::max(rgb_max, *rgb_error_img_it);
                  depth_max = std::max(depth_max, *depth_error_img_it);
                }
                *weight_img_it = *w_it;
                err_img_it->val[2] = (*r_it)(0);
                err_img_it->val[1] = (*r_it)(1);
                *erri_img_it = (*r_it)(0);
                *errd_img_it = (*r_it)(1);
                ++w_it;
                ++r_it;
              }

              ++valid_it;
            }
          }
          cv::Mat left(video_frame, cv::Rect(0, 0, s.width, s.height));
          cv::Mat right(video_frame, cv::Rect(s.width, 0, s.width, s.height));

          cv::resize(debug_img, debug_img_scaled, s);
          cv::resize(weight_img, weight_img_scaled, s);

          cv::resize(rgb_error_img / rgb_max, left, left.size());
          cv::resize(depth_error_img, right, right.size());

          std::stringstream ss;
          ss << "iteration: " << std::setw(2) << itctx_.Iteration;// << " log likelihood: " << std::setiosflags(std::ios::fixed) << std::setw(7) << std::setprecision(0) << ll;

          cv::putText(video_frame, ss.str(), cv::Point(int(0.05*s.width), int(0.95*s.height)), CV_FONT_HERSHEY_DUPLEX, 0.7, cv::Scalar(1.0), 1, 1, false);

          video_frame.convertTo(video_frame_u8, CV_8UC1, 255.0);


          for(int i = 0; i < 10; ++i)
            vw << video_frame_u8;
          */
          //cv::imshow("points", debug_img_scaled);
          //cv::imshow("weights", weight_img_scaled);
          //cv::imshow("error", video_frame);
          //cv::waitKey(100);
        }

          itctx_.LastError = itctx_.Error;
          itctx_.Error = total_error;

//      sw_error[itctx_.Level].stopAndPrint();

      // accept the last increment?
      accept = itctx_.Error < itctx_.LastError;

      if(!accept)
      {
        initial.revert();
        estimate.revert();

        break;
      }

      // now build equation system
//      sw_linsys[itctx_.Level].start();

      WeightVectorType::iterator w_it = weights.begin();

      Matrix2x6 J, Jw;
      Eigen::Vector2f Ji;
      Vector6 Jz;
      ls.initialize(1);
      for(PointIterator e_it = compute_residuals_result.first_point_error; e_it != compute_residuals_result.last_point_error; ++e_it, ++w_it)
      {
        computeJacobianOfProjectionAndTransformation(e_it->getPointVec4f(), Jw);
        compute3rdRowOfJacobianOfTransformation(e_it->getPointVec4f(), Jz);

        J.row(0) = e_it->getIntensityDerivativeVec2f().transpose() * Jw;
        J.row(1) = e_it->getDepthDerivativeVec2f().transpose() * Jw - Jz.transpose();

        ls.update(J, e_it->getIntensityAndDepthVec2f(), (*w_it) * precision);
      }
      ls.finish();

      A = ls.A.cast<double>() + cfg.Mu * Matrix6d::Identity();
      b = ls.b.cast<double>() + cfg.Mu * initial().log();
      x = A.ldlt().solve(b);

//      sw_linsys[itctx_.Level].stopAndPrint();

      iteration_stats.EstimateIncrement = x;
      iteration_stats.EstimateInformation = A;

      itctx_.Iteration++;
//      sw_it[itctx_.Level].stopAndPrint();
    }
    while(accept && x.lpNorm<Eigen::Infinity>() > cfg.Precision && !itctx_.IterationsExceeded());

    if(!accept)
      level_stats.TerminationCriterion = TerminationCriteria::LogLikelihoodDecreased;

    if(x.lpNorm<Eigen::Infinity>() <= cfg.Precision)
      level_stats.TerminationCriterion = TerminationCriteria::IncrementTooSmall;

    if(itctx_.IterationsExceeded())
      level_stats.TerminationCriterion = TerminationCriteria::IterationsExceeded;

//    sw_level[itctx_.Level].stopAndPrint();
  }

  LevelStats& last_level = result.Statistics.Levels.back();
  IterationStats& last_iteration = last_level.TerminationCriterion != TerminationCriteria::LogLikelihoodDecreased ? last_level.Iterations[last_level.Iterations.size() - 1] : last_level.Iterations[last_level.Iterations.size() - 2];

  result.Transformation = estimate().inverse().matrix();
  result.Information = last_iteration.EstimateInformation * 0.008 * 0.008;
  result.LogLikelihood = last_iteration.TDistributionLogLikelihood + last_iteration.PriorLogLikelihood;

  return success;
}

// jacobian computation
inline void DenseTracker::computeJacobianOfProjectionAndTransformation(const Vector4& p, Matrix2x6& j)
{
  NumType z = 1.0f / p(2);
  NumType z_sqr = 1.0f / (p(2) * p(2));

  j(0, 0) =  z;
  j(0, 1) =  0.0f;
  j(0, 2) = -p(0) * z_sqr;
  j(0, 3) = j(0, 2) * p(1);//j(0, 3) = -p(0) * p(1) * z_sqr;
  j(0, 4) = 1.0f - j(0, 2) * p(0);//j(0, 4) =  (1.0 + p(0) * p(0) * z_sqr);
  j(0, 5) = -p(1) * z;

  j(1, 0) =  0.0f;
  j(1, 1) =  z;
  j(1, 2) = -p(1) * z_sqr;
  j(1, 3) = -1.0f + j(1, 2) * p(1); //j(1, 3) = -(1.0 + p(1) * p(1) * z_sqr);
  j(1, 4) = -j(0, 3); //j(1, 4) =  p(0) * p(1) * z_sqr;
  j(1, 5) =  p(0) * z;
}

inline void DenseTracker::compute3rdRowOfJacobianOfTransformation(const Vector4& p, Vector6& j)
{
  j(0) = 0.0;
  j(1) = 0.0;
  j(2) = 1.0;
  j(3) = p(1);
  j(4) = -p(0);
  j(5) = 0.0;
}

} /* namespace dvo */
