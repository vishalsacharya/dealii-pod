/* ---------------------------------------------------------------------
 * Copyright (C) 2014-2015 David Wells
 *
 * This file is NOT part of the deal.II library.
 *
 * This file is free software; you can use it, redistribute it, and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE at
 * the top level of the deal.II distribution.
 *
 * ---------------------------------------------------------------------
 * This program is based on step-26 of the deal.ii library.
 *
 * Author: David Wells, Virginia Tech, 2014
 * Author: David Wells, Rensselaer Polytechnic Institute, 2015
 */
#ifndef dealii__rom_ns_parameters_h
#define dealii__rom_ns_parameters_h
#include <deal.II/base/parameter_handler.h>

#include <fstream>

using namespace dealii;
namespace POD
{
  enum class FilterModel
    {
      Differential,
      L2Projection,
      PostDifferentialFilter,
      PostL2ProjectionFilter,
      PostDifferentialFilterRelax,
      LerayHybrid,
      ADLavrentiev,
      ADTikonov
    };

  namespace NavierStokes
  {
    class Parameters
    {
    public:
      double reynolds_n;

      POD::FilterModel filter_model;
      double noise_multiplier;
      double lavrentiev_parameter;
      double relaxation_parameter;
      double filter_radius;
      unsigned int cutoff_n;
      bool filter_mean;

      unsigned int n_pod_dofs;
      double initial_time;
      double final_time;
      double time_step;

      int output_interval;

      bool test_output;

      void read_data(const std::string &file_name);
    private:
      void configure_parameter_handler(ParameterHandler &parameter_handler) const;
    };
  }
}
#endif
