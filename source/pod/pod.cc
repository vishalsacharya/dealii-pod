#include <deal.II-pod/extra/extra.h>

#include <deal.II-pod/h5/h5.h>

#include <deal.II-pod/pod/pod.h>
#include <deal.II-pod/pod/pod.templates.h>

namespace POD
{
  using namespace dealii;

  void load_pod_basis(const std::string                &pod_vector_glob,
                      const std::string                &mean_vector_file_name,
                      BlockVector<double>              &mean_vector,
                      std::vector<BlockVector<double>> &pod_vectors)
  {
    for (auto &file_name : extra::expand_file_names(pod_vector_glob))
      {
        BlockVector<double> block_vector;
        H5::load_block_vector(file_name, block_vector);
        pod_vectors.push_back(std::move(block_vector));
      }
    H5::load_block_vector(mean_vector_file_name, mean_vector);
  }


  void method_of_snapshots(const SparseMatrix<double>     &mass_matrix,
                           const std::vector<std::string> &snapshot_file_names,
                           const unsigned int              n_pod_vectors,
                           const bool                      center_trajectory,
                           BlockPODBasis                  &pod_basis)
  {
    std::vector<BlockVector<double>> snapshots;

    const double mean_weight = 1.0/snapshot_file_names.size();
    bool pod_basis_initialized = false;
    const unsigned int n_snapshots = snapshot_file_names.size();
    unsigned int n_dofs_per_block = 0;
    unsigned int n_blocks = 0;
    unsigned int i = 0;

    for (const std::string &snapshot_file_name : snapshot_file_names)
      {
        BlockVector<double> block_vector;
        H5::load_block_vector(snapshot_file_name, block_vector);
        if (!pod_basis_initialized)
          {
            n_blocks = block_vector.n_blocks();
            Assert(n_blocks > 0, ExcInternalError());
            n_dofs_per_block = block_vector.block(0).size();
            pod_basis.reinit(n_blocks, n_dofs_per_block);
            pod_basis_initialized = true;
          }
        Assert(block_vector.n_blocks() == n_blocks, ExcIO());
        pod_basis.mean_vector.add(mean_weight, block_vector);
        snapshots.push_back(std::move(block_vector));
      }

    if (center_trajectory)
      {
        for (BlockVector<double> &snapshot : snapshots)
          {
            snapshot.add(-1.0, pod_basis.mean_vector);
          }
      }
    else
      {
        pod_basis.mean_vector = 0.0;
      }

    LAPACKFullMatrix<double> correlation_matrix(n_snapshots);
    LAPACKFullMatrix<double> identity(n_snapshots);
    identity = 0.0;
    BlockVector<double> temp(n_blocks, n_dofs_per_block);
    for (unsigned int row = 0; row < n_snapshots; ++row)
      {
        for (unsigned int block_n = 0; block_n < n_blocks; ++block_n)
          {
            mass_matrix.vmult(temp.block(block_n), snapshots.at(row).block(block_n));
          }
        for (unsigned int column = 0; column <= row; ++column)
          {
            const double value = temp * snapshots[column];
            correlation_matrix(row, column) = value;
            correlation_matrix(column, row) = value;
          }
        identity(row, row) = 1.0;
      }

    std::vector<Vector<double>> eigenvectors(n_snapshots);
    correlation_matrix.compute_generalized_eigenvalues_symmetric(identity,
        eigenvectors);
    pod_basis.singular_values.resize(n_snapshots);
    for (i = 0; i < n_snapshots; ++i)
      {
        // As the matrix has provably positive real eigenvalues...
        const std::complex<double> eigenvalue = correlation_matrix.eigenvalue(i);
        Assert(eigenvalue.imag() == 0.0, ExcInternalError());
        pod_basis.singular_values[i] = std::sqrt(eigenvalue.real());
      }
    std::reverse(eigenvectors.begin(), eigenvectors.end());
    std::reverse(pod_basis.singular_values.begin(), pod_basis.singular_values.end());

    const unsigned int n_actual_pod_vectors = std::min(n_snapshots, n_pod_vectors);
    pod_basis.vectors.resize(n_actual_pod_vectors);

    {
      Threads::TaskGroup<> linear_combination_tasks;
      for (unsigned int eigenvector_n = 0; eigenvector_n < n_actual_pod_vectors;
           ++eigenvector_n)
        {
          linear_combination_tasks += Threads::new_task
            (std::function<void()>([&, eigenvector_n]
             {
               const Vector<double> &eigenvector = eigenvectors[eigenvector_n];
               const double singular_value = pod_basis.singular_values[eigenvector_n];

               BlockVector<double> pod_vector(n_blocks, n_dofs_per_block);
               std::size_t snapshot_n = 0;
               for (const BlockVector<double> &snapshot : snapshots)
                 {
                   if (!std::isnan(eigenvector[snapshot_n]) && !std::isnan(singular_value))
                     {
                       pod_vector.add(eigenvector[snapshot_n], snapshot);
                     }
                   ++snapshot_n;
                 }
               pod_basis.vectors[eigenvector_n] = std::move(pod_vector);
             }));
        }
    }

    for (unsigned int pod_vector_n = 0; pod_vector_n < n_actual_pod_vectors; ++pod_vector_n)
      {
        const double singular_value = pod_basis.singular_values.at(pod_vector_n);
        if (!std::isnan(singular_value))
          {
            pod_basis.vectors.at(pod_vector_n) *= 1.0/singular_value;
          }
      }
  }


  BlockPODBasis::BlockPODBasis() : n_blocks(0), n_dofs_per_block(0) {}

  BlockPODBasis::BlockPODBasis(unsigned int n_blocks, unsigned int n_dofs_per_block) :
    n_blocks(n_blocks), n_dofs_per_block(n_dofs_per_block)
  {
    mean_vector.reinit(n_blocks, n_dofs_per_block);
    mean_vector.collect_sizes();
  }

  void BlockPODBasis::reinit(unsigned int n_blocks, unsigned int n_dofs_per_block)
  {
    this->n_blocks = n_blocks;
    this->n_dofs_per_block = n_dofs_per_block;
    mean_vector.reinit(n_blocks, n_dofs_per_block);
    mean_vector.collect_sizes();
    mean_vector = 0;
  }

  unsigned int BlockPODBasis::get_n_pod_vectors() const
  {
    return vectors.size();
  }

  void BlockPODBasis::project_load_vector(BlockVector<double> &load_vector,
                                          BlockVector<double> &pod_load_vector) const
  {
    (void)load_vector;
    (void)pod_load_vector;
    Assert(false, StandardExceptions::ExcNotImplemented());
  }

  void BlockPODBasis::project_to_fe(const BlockVector<double> &pod_vector,
                                    BlockVector<double>       &fe_vector) const
  {
    (void)pod_vector;
    (void)fe_vector;
    Assert(false, StandardExceptions::ExcNotImplemented());

  }

  void create_reduced_matrix(const std::vector<BlockVector<double>> &pod_vectors,
                             const SparseMatrix<double>             &full_matrix,
                             FullMatrix<double>                     &rom_matrix)
  {
    std::vector<unsigned int> dims;
    for (unsigned int i = 0; i < pod_vectors.at(0).n_blocks(); ++i)
      {
        dims.push_back(i);
      }
    create_reduced_matrix(pod_vectors, full_matrix, dims, rom_matrix);
  }


  void create_reduced_matrix(const std::vector<BlockVector<double>> &pod_vectors,
                             const SparseMatrix<double>             &full_matrix,
                             const std::vector<unsigned int>        &dims,
                             FullMatrix<double>                     &rom_matrix)
  {
    const unsigned int n_dofs = pod_vectors[0].block(0).size();
    const unsigned int n_pod_dofs = pod_vectors.size();
    rom_matrix.reinit(n_pod_dofs, n_pod_dofs);
    rom_matrix = 0.0;
    Vector<double> temp(n_dofs);
    for (auto dim_n : dims)
      {
        for (unsigned int column = 0; column < n_pod_dofs; ++column)
          {
            full_matrix.vmult(temp, pod_vectors.at(column).block(dim_n));
            for (unsigned int row = 0; row < n_pod_dofs; ++row)
              {
                rom_matrix(row, column) += pod_vectors.at(row).block(dim_n) * temp;
              }
          }
      }
  }

  template class PODOutput<2>;

  template class PODOutput<3>;

  template
  void create_dof_handler_from_triangulation_file
  (const std::string &file_name,
   const bool        &renumber,
   const FE_Q<2>     &fe,
   DoFHandler<2>     &dof_handler,
   Triangulation<2>  &triangulation);

  template
  void create_dof_handler_from_triangulation_file
  (const std::string &file_name,
   const bool        &renumber,
   const FE_Q<3>     &fe,
   DoFHandler<3>     &dof_handler,
   Triangulation<3>  &triangulation);
}
