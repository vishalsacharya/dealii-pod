#include <deal.II/base/utilities.h>
#include <deal.II/base/quadrature_lib.h>

#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/vector.h>

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_in.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>

#include <deal.II/numerics/matrix_tools.h>

#include <string>
#include <memory>
#include <vector>

#include <deal.II-pod/ns/filter.h>
#include <deal.II-pod/ns/ns.h>
#include <deal.II-pod/extra/extra.h>
#include <deal.II-pod/pod/pod.h>
#include <deal.II-pod/h5/h5.h>

#include "parameters.h"

namespace ComputePOD
{
  using namespace dealii;
  using namespace POD;

  template<int dim>
  class ComputePODMatrices
  {
  public:
    ComputePODMatrices(const Parameters &parameters);
    void run();
  private:
    void load_pod_vectors();

    void setup_mass_matrix();
    void setup_laplace_matrix();
    void setup_boundary_matrix();
    void setup_advective_linearization_matrix();
    void setup_gradient_linearization_matrix();
    void setup_nonlinearity();

    void save_rom_components();

    const Parameters parameters;

    FE_Q<dim> fe;
    QGauss<dim> quad;
    Triangulation<dim> triangulation;
    SparsityPattern sparsity_pattern;
    DoFHandler<dim> dof_handler;

    // for Leray models: otherwise, these point to the unfiltered versions
    std::shared_ptr<std::vector<BlockVector<double>>> filtered_pod_vectors;
    std::shared_ptr<BlockVector<double>> filtered_mean_vector;

    std::shared_ptr<std::vector<BlockVector<double>>> pod_vectors;
    std::shared_ptr<BlockVector<double>> mean_vector;
    unsigned int n_dofs;

    FullMatrix<double> mass_matrix;
    FullMatrix<double> laplace_matrix;
    FullMatrix<double> boundary_matrix;

    FullMatrix<double> gradient_matrix;
    FullMatrix<double> advection_matrix;

    std::vector<FullMatrix<double>> nonlinearity;

    Vector<double> mean_contribution;
    Vector<double> initial;
  };

  template<int dim>
  ComputePODMatrices<dim>::ComputePODMatrices
  (const Parameters &params)
    :
    parameters(params),
    fe(params.fe_order),
    quad(params.fe_order + 2),
    filtered_pod_vectors {std::make_shared<std::vector<BlockVector<double>>>()},
    filtered_mean_vector {std::make_shared<BlockVector<double>>()},
    pod_vectors {std::make_shared<std::vector<BlockVector<double>>>()},
    mean_vector {std::make_shared<BlockVector<double>>()}
  {
    POD::create_dof_handler_from_triangulation_file
    (parameters.triangulation_file_name, parameters.renumber, fe, dof_handler,
     triangulation);

    DynamicSparsityPattern d_sparsity(dof_handler.n_dofs());
    DoFTools::make_sparsity_pattern(dof_handler, d_sparsity);
    sparsity_pattern.copy_from(d_sparsity);
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::load_pod_vectors()
  {
    // TODO replace hardcoded strings with parameter values
    POD::load_pod_basis("pod-vector*.h5", "mean-vector.h5", *mean_vector,
                        *pod_vectors);
    AssertThrow(pod_vectors->size() >= parameters.n_pod_vectors,
                ExcMessage("The number of specified POD vectors exceeds the "
                           "number of POD vectors found in the current directory."));
    n_dofs = pod_vectors->at(0).block(0).size();
    pod_vectors->resize(parameters.n_pod_vectors);

    mean_contribution.reinit(pod_vectors->size());

    // This is an abuse of notation to save duplication: if the POD vectors are
    // not filtered, then simply assign the filtered pod vectors pointer to
    // point to the unfiltered ones.
    if (parameters.use_leray_regularization
        && parameters.filter_radius != 0.0)
      {
        std::shared_ptr<SparseMatrix<double>> full_mass_matrix
          {new SparseMatrix<double>};
        full_mass_matrix->reinit(sparsity_pattern);
        SparseMatrix<double> full_laplace_matrix(sparsity_pattern);
        SparseMatrix<double> full_boundary_matrix(sparsity_pattern);
        QGauss<dim - 1> face_quad(fe.degree + 3);
        MatrixCreator::create_mass_matrix(dof_handler, quad, *full_mass_matrix);
        MatrixCreator::create_laplace_matrix
          (dof_handler, quad, full_laplace_matrix);
        POD::NavierStokes::create_boundary_matrix
          (dof_handler, face_quad, parameters.outflow_label, full_boundary_matrix);

        Leray::LerayFilter filter
          (parameters.filter_radius, full_mass_matrix, full_boundary_matrix,
           full_laplace_matrix);

        // TODO support not filtering the mean
        filter.apply(*filtered_mean_vector, *mean_vector);

        filtered_pod_vectors->resize(pod_vectors->size());
        for (unsigned int pod_vector_n = 0; pod_vector_n < pod_vectors->size();
             ++pod_vector_n)
          {
            filter.apply(filtered_pod_vectors->at(pod_vector_n),
                         pod_vectors->at(pod_vector_n));
          }
      }
    else
      {
        filtered_pod_vectors = pod_vectors;
        filtered_mean_vector = mean_vector;
      }
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::setup_mass_matrix()
  {
    // This function is a slight misnomer: I also project the initial
    // condition here too.
    SparseMatrix<double> full_mass_matrix(sparsity_pattern);
    MatrixCreator::create_mass_matrix(dof_handler, quad, full_mass_matrix);
    POD::create_reduced_matrix(*pod_vectors, full_mass_matrix, mass_matrix);

    BlockVector<double> centered_initial;
    // TODO replace hardcoded string with a parameter value
    H5::load_block_vector("initial.h5", centered_initial);
    initial.reinit(pod_vectors->size());
    centered_initial -= *mean_vector;
    for (unsigned int dim_n = 0; dim_n < dim; ++dim_n)
      {
        Vector<double> temp(n_dofs);
        full_mass_matrix.vmult(temp, centered_initial.block(dim_n));
        for (unsigned int pod_vector_n = 0; pod_vector_n < pod_vectors->size();
             ++pod_vector_n)
          {
            initial[pod_vector_n] +=
              temp * pod_vectors->at(pod_vector_n).block(dim_n);
          }
      }
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::setup_laplace_matrix()
  {
    // also a misnomer: this sets up the Laplace matrix *and* subtracts off
    // the relevant part from the mean contribution.
    SparseMatrix<double> full_laplace_matrix(sparsity_pattern);
    MatrixCreator::create_laplace_matrix(dof_handler, quad, full_laplace_matrix);
    POD::create_reduced_matrix(*pod_vectors, full_laplace_matrix, laplace_matrix);

    for (unsigned int dim_n = 0; dim_n < dim; ++dim_n)
      {
        Vector<double> temp(n_dofs);
        full_laplace_matrix.vmult(temp, mean_vector->block(dim_n));
        for (unsigned int pod_vector_n = 0; pod_vector_n < pod_vectors->size(); ++pod_vector_n)
          {
            mean_contribution[pod_vector_n] -= 1.0/parameters.reynolds_n*
              (temp * pod_vectors->at(pod_vector_n).block(dim_n));
          }
      }
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::setup_boundary_matrix()
  {
    // Same as above: setup the matrix and subtract off the relevant piece of
    // the mean contribution vector.
    SparseMatrix<double> full_boundary_matrix(sparsity_pattern);
    QGauss<dim - 1> face_quad(fe.degree + 2);
    POD::NavierStokes::create_boundary_matrix
      (dof_handler, face_quad, parameters.outflow_label, full_boundary_matrix);

    std::vector<unsigned int> dims {0};
    POD::create_reduced_matrix(*pod_vectors, full_boundary_matrix, dims,
                               boundary_matrix);

    Vector<double> temp(n_dofs);
    full_boundary_matrix.vmult(temp, mean_vector->block(0));
    for (unsigned int pod_vector_n = 0; pod_vector_n < pod_vectors->size(); ++pod_vector_n)
      {
        mean_contribution[pod_vector_n] += 1.0/parameters.reynolds_n
          *(temp * pod_vectors->at(pod_vector_n).block(0));
      }
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::setup_advective_linearization_matrix()
  {
    QGauss<dim> higher_quadrature(2*(parameters.fe_order + 1));
    POD::NavierStokes::create_reduced_advective_linearization
    (dof_handler, sparsity_pattern, higher_quadrature, *filtered_mean_vector,
     *pod_vectors, advection_matrix);
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::setup_gradient_linearization_matrix()
  {
    QGauss<dim> higher_quadrature(2*(parameters.fe_order + 1));
    POD::NavierStokes::create_reduced_gradient_linearization
    (dof_handler, sparsity_pattern, higher_quadrature, *mean_vector, *pod_vectors,
     *filtered_pod_vectors, gradient_matrix);
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::setup_nonlinearity()
  {
    QGauss<dim> higher_quadrature(2*(parameters.fe_order + 1));

    Vector<double> nonlinear_contribution(pod_vectors->size());
    POD::NavierStokes::create_nonlinear_centered_contribution
      (dof_handler, sparsity_pattern, higher_quadrature, *mean_vector,
       *mean_vector, *pod_vectors, nonlinear_contribution);
    mean_contribution.add(-1.0, nonlinear_contribution);

    POD::NavierStokes::create_reduced_nonlinearity
    (dof_handler, sparsity_pattern, higher_quadrature, *pod_vectors,
     *filtered_pod_vectors, nonlinearity);
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::save_rom_components()
  {
    if (parameters.test_output)
      {
        // For some reason the debug build computes slightly (!!!) different
        // values than the release build, so compare it with a higher
        // tolerance.
#ifdef DEBUG
        constexpr double tolerance {1e-13};
#else
        constexpr double tolerance {0.0};
#endif
#define TEST_MATRIX(EXP)                                                                \
        {                                                                               \
          FullMatrix<double> test_##EXP;                                                \
          H5::load_full_matrix("rom-" #EXP "-matrix.h5", test_##EXP);                   \
          const bool are_equal = extra::are_equal(EXP##_matrix, test_##EXP, tolerance); \
          AssertThrow(are_equal, ExcMessage("Test failed! The " #EXP                    \
                                            " matrices are not the same."));            \
        }

        TEST_MATRIX(mass)
        TEST_MATRIX(laplace)
        TEST_MATRIX(boundary)
        TEST_MATRIX(gradient)
        TEST_MATRIX(advection)
#undef TEST_MATRIX
#define TEST_VECTOR(FILE_NAME, VECTOR_NAME)                                                    \
        {                                                                                      \
          Vector<double> test_##VECTOR_NAME;                                                   \
          H5::load_vector(#FILE_NAME, test_##VECTOR_NAME);                                     \
          const bool are_equal = extra::are_equal(VECTOR_NAME, test_##VECTOR_NAME, tolerance); \
          AssertThrow(are_equal, ExcMessage("Test failed! The " #VECTOR_NAME                   \
          " vectors are not the same."));                                                      \
        }

        TEST_VECTOR(rom-initial-condition.h5, initial)
        TEST_VECTOR(rom-mean-contribution.h5, mean_contribution)
#undef TEST_VECTOR

        std::vector<FullMatrix<double>> test_nonlinearity;
        H5::load_full_matrices("rom-nonlinearity.h5", test_nonlinearity);

          for (unsigned int i = 0; i < pod_vectors->size(); ++i)
            {
              AssertThrow(extra::are_equal(test_nonlinearity[i], nonlinearity[i], tolerance),
                          ExcMessage("Test failed! The nonlinearity is not the same as "
                                     "the saved version."));
            }
      }
    else
      {
        H5::save_full_matrix("rom-mass-matrix.h5", mass_matrix);
        H5::save_full_matrix("rom-laplace-matrix.h5", laplace_matrix);
        H5::save_full_matrix("rom-boundary-matrix.h5", boundary_matrix);
        H5::save_full_matrix("rom-gradient-matrix.h5", gradient_matrix);
        H5::save_full_matrix("rom-advection-matrix.h5", advection_matrix);
        H5::save_vector("rom-mean-contribution.h5", mean_contribution);
        H5::save_vector("rom-initial-condition.h5", initial);
        H5::save_full_matrices("rom-nonlinearity.h5", nonlinearity);
      }
  }



  template<int dim>
  void
  ComputePODMatrices<dim>::run()
  {
    load_pod_vectors();
    setup_mass_matrix();
    setup_laplace_matrix();
    setup_boundary_matrix();
    setup_advective_linearization_matrix();
    setup_gradient_linearization_matrix();
    setup_nonlinearity();
    save_rom_components();
  }
}




int main(int argc, char **argv)
{
  using namespace POD;
  Utilities::MPI::MPI_InitFinalize mpi_initialization
  (argc, argv, numbers::invalid_unsigned_int);
  {
    ComputePOD::Parameters parameters;
    parameters.read_data("parameters.prm");
    if (parameters.dimension == 2)
      {
        ComputePOD::ComputePODMatrices<2> pod_matrices(parameters);
        pod_matrices.run();
      }
    else
      {
        ComputePOD::ComputePODMatrices<3> pod_matrices(parameters);
        pod_matrices.run();
      }
  }
}
