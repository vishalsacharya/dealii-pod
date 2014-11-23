#include "pod.h"

namespace POD
{
  using namespace dealii::PETScWrappers;
  template<typename U, typename T>
  void copy_vector(const U &src, unsigned int start_src,
                   unsigned int start_dst, unsigned int total,
                   T &dst)
  {
    for (unsigned int index = 0; index < total; ++index)
      {
        dst[start_dst + index] = src[start_src + index];
      }

  }

  void method_of_snapshots(dealii::SparseMatrix<double> &mass_matrix,
                           std::vector<std::string> &snapshot_file_names,
                           std::string &dataset_name,
                           unsigned int n_pod_vectors,
                           BlockPODBasis &pod_basis)
  {
    dealii::BlockVector<double> block_vector;
    std::vector< dealii::BlockVector<double> > snapshots;

    const double mean_weight = 1.0/snapshot_file_names.size();
    bool pod_basis_initialized = false;
    const int n_snaphots = snapshot_file_names.size();
    int n_dofs_per_block = 0;
    unsigned int i = 0;
    for (auto &snapshot_file : snapshot_file_names)
      {
        load_hdf5(snapshot_file, dataset_name, block_vector);
        if (!pod_basis_initialized)
          {
            n_dofs_per_block = block_vector.block(0).size();
            n_blocks = block_vector.n_blocks();
            pod_basis.reinit(n_blocks, n_dofs_per_block);
            pod_basis_initialized = true;
          }
        pod_basis.mean_vector.sadd(0.0, mean_weight, block_vector);
        snapshots.insert(i, std::move(block_vector));
        ++i;
      }


    // center the trajectory.
    for (auto& snapshot : snapshots)
      {
        snapshot -= pod_basis.mean_vector;
      }

    dealii::LAPACKFullMatrix<double> correlation_matrix(n_snaphots);
    dealii::LAPACKFullMatrix<double> identity(n_snaphots);
    identity = 0.0;
    dealii::Vector<double> temp(pod_basis.mean_vector.block(0).size());
    for (unsigned int row = 0; row < n_snapshots; ++row)
      {
        for (unsigned int column = 0; column <= row; ++column)
          {
            double value = 0;
            for (unsigned int block_n = 0; block_n < mean_block.size();
                 ++block_n)
              {
                mass_matrix.vmult(temp, snapshots[i].block(block_n));
                value += temp * snapshots[j].block(block_n);
              }
            correlation_matrix(row, column) = value;
            correlation_matrix(column, row) = value;
          }
        identity(row, row) = 1.0;
      }

    dealii::Vector<double> eigenvectors(n_pod_vectors);
    correlation_matrix.compute_eigenvalues_symmetric(identity, eigenvectors);
    for (unsigned int i = 0; i < n_pod_vectors; ++i)
      {
        // As the matrix has provably positive real eigenvalues...
        std::complex<double> eigenvalue = correlation_matrix.eigenvalue(i);
        Assert(eigenvalue.imag() == 0.0, dealii::ExcInternalError());
        Assert(eigenvalue.real() > 0.0, dealii::ExcInternalError());
        pod_basis.singular_values.insert(i, std::sqrt(eigenvalue.real()));
      }

    unsigned int pod_vector_n = 0;
    for (auto &eigenvector : eigenvectors)
      {
        block_vector = 0;
        i = 0;
        for (auto &snapshot : snapshots)
          {
            block_vector.sadd(0.0, eigenvector[i], snapshot);
            ++i;
          }
        pod_basis.pod_vectors.insert(pod_vector_n, std::move(block_vector));
        ++pod_vector_n;
      }
  }

  class EigenvalueMethod : public dealii::PETScWrappers::MatrixFree
  // Class for computing POD vectors by the eigenproblem
  //
  // Y^T Y M v = l v
  //
  // where Y is the matrix of snapshots (each row is one snapshot), M is the
  // mass matrix, v is a POD vector, and l is a singular value.
  //
  // Perhaps this was a premature optimization, but I did not use the usual
  // convention of one snapshot per column to greatly speed up the reading of
  // the snapshot matrix.
  {
  public:
    EigenvalueMethod(dealii::SparseMatrix<double> &mass_matrix,
                     dealii::FullMatrix<double> &snapshots,
                     unsigned int num_blocks)
      : num_blocks (num_blocks), mass_matrix (mass_matrix), snapshots (snapshots)
    {
      // use the reinit function to set the number of rows and columns. This is,
      // AFAIK, the only way to set the underlying PETSc matrix representation's
      // values.
      const unsigned int local_rows = this->mass_matrix.m()*num_blocks;
      const unsigned int local_columns = this->mass_matrix.n()*num_blocks;
      reinit(local_rows, local_columns, local_rows, local_columns);
    }

    void vmult(VectorBase &dst, const VectorBase &src) const
    {
      dealii::Vector<double> tmp_component_src(src.size()/num_blocks);
      dealii::Vector<double> tmp_component_dst(src.size()/num_blocks);
      dealii::Vector<double> tmp_short(snapshots.m());
      dealii::Vector<double> tmp_long(snapshots.n());
      dealii::Vector<double> tmp_dst(dst.size());

      // TODO this requires *a lot* of extra copying. There may be a better way to do
      // this with VectorView.
      for (unsigned int j = 0; j < num_blocks; ++j)
        {
          copy_vector(src, 0, 0, src.size()/num_blocks, tmp_component_src);
          mass_matrix.vmult(tmp_component_dst, tmp_component_src);
          copy_vector(tmp_component_src, 0, j*tmp_long.size()/num_blocks,
                      tmp_component_src.size(), tmp_long);
        }
      snapshots.vmult(tmp_short, tmp_long);
      snapshots.Tvmult(tmp_dst, tmp_short);
      copy_vector(tmp_dst, 0, 0, dst.size(), dst);
    }

    void vmult_add(VectorBase &dst, const VectorBase &src) const
    {
      Assert(false, dealii::ExcNotImplemented());
    }

    void Tvmult(VectorBase &dst, const VectorBase &src) const
    {
      Assert(false, dealii::ExcNotImplemented());
    }

    void Tvmult_add(VectorBase &dst, const VectorBase &src) const
    {
      Assert(false, dealii::ExcNotImplemented());
    }
    unsigned int num_blocks;

  private:
    dealii::SparseMatrix<double> &mass_matrix;
    dealii::FullMatrix<double> &snapshots;
  };

  class BlockPODBasis
  {
  public:
    BlockPODBasis(unsigned int n_blocks, unsigned int n_dofs_per_block);
    std::map< int, dealii::BlockVector<double> > vectors;
    dealii::BlockVector<double> mean_vector;
    std::vector<double> singular_values;
    unsigned int get_num_pod_vectors() const;
    void reinit(unsigned int n_blocks, unsigned int n_dofs_per_block);
    void project_load_vector(dealii::Vector<double> &load_vector,
                             dealii::Vector<double> &pod_load_vector);
    void project_to_fe(const dealii::Vector<double> &pod_vector,
                       dealii::Vector<double> &fe_vector) const;
  private:
    unsigned int n_blocks;
    unsigned int n_dofs_per_block;
  };

  BlockPODBasis::BlockPODBasis(unsigned int n_blocks, unsigned int n_dofs_per_block) :
    n_blocks(n_dofs), n_dofs_per_block(n_dofs_per_block) {}

  void BlockPODBasis::reinit(unsigned int n_blocks, unsigned int n_dofs_per_block)
  {
    this->n_blocks = n_blocks;
    this->n_dofs_per_block = n_dofs_per_block;
    mean_vector.reinit(n_blocks, n_dofs_per_block);
  }

  unsigned int BlockPODBasis::get_n_pod_vectors() const
    {
      return singular_values.size();
    }

  void BlockPODBasis::project_load_vector(dealii::BlockVector<double> &load_vector,
                                          dealii::BlockVector<double> &pod_load_vector)
  {
    ;
  }

  void BlockPODBasis::project_to_fe(const dealii::BlockVector<double> &pod_vector,
                                    dealii::BlockVector<double> &fe_vector)
    {

    }

  class PODBasis
  {
  public:
    std::map<int, dealii::Vector<double>> vectors;
    std::vector<double> singular_values;
    unsigned int get_num_pod_vectors() const;
    void project_load_vector(dealii::Vector<double> &load_vector,
                             dealii::Vector<double> &pod_load_vector);
    void project_to_fe(const dealii::Vector<double> &pod_vector,
                       dealii::Vector<double> &fe_vector) const;
  };

  unsigned int PODBasis::get_num_pod_vectors() const
  {
    return singular_values.size();
  }

  void PODBasis::project_load_vector(dealii::Vector<double> &load_vector,
                                     dealii::Vector<double> &pod_load_vector)
  {
    pod_load_vector.reinit(get_num_pod_vectors());
    for (unsigned int j = 0; j < get_num_pod_vectors(); ++j)
      {
        pod_load_vector[j] = vectors[j] * load_vector;
      }
  }

  void PODBasis::project_to_fe(const dealii::Vector<double> &pod_vector,
                               dealii::Vector<double> &fe_vector) const
  {
    fe_vector.reinit(vectors.at(0).size());
    for (unsigned int j = 0; j < this->get_num_pod_vectors(); ++j)
      {
        fe_vector.add(pod_vector[j], vectors.at(j));
      }
  }

  void pod_basis(dealii::SparseMatrix<double> &mass_matrix,
                 dealii::FullMatrix<double> &snapshot_matrix,
                 const unsigned int num_pod_vectors,
                 PODBasis &result)
  {
    Assert(num_pod_vectors > 0, dealii::ExcInternalError());
    unsigned int num_blocks = 3;
    EigenvalueMethod pod_data(mass_matrix, snapshot_matrix, num_blocks);
    dealii::SolverControl solver_control (mass_matrix.m(), 5e-6);
    dealii::SLEPcWrappers::SolverArnoldi eigensolver (solver_control);
    eigensolver.set_which_eigenpairs(EPS_LARGEST_MAGNITUDE);
    eigensolver.set_problem_type(EPS_HEP);

    std::vector<Vector> eigenvectors;
    for (unsigned int i = 0; i < num_pod_vectors; ++i)
      {
        eigenvectors.push_back(Vector(pod_data.m()));
      }
    eigensolver.solve(pod_data, result.singular_values, eigenvectors,
                      num_pod_vectors);

    std::cout << "Finished computing eigenvalues." << std::endl;
    for (int i = num_pod_vectors - 1; i >= 0; --i)
      {
        // Note that we previously computed the *eigenvalues*, not the singular values.
        auto eigenvalue = result.singular_values[i];
        result.singular_values[i] = std::sqrt(eigenvalue);
        // TODO this is an icky bit of copying, but there does not seem to be a built
        // in for it.
        dealii::Vector<double> outvalues(eigenvectors[i].size());
        for (unsigned int j = 0; j < eigenvectors[i].size(); ++j)
          {
            outvalues[j] = eigenvectors[i][j];
          }
        result.vectors.insert(std::pair<int, dealii::Vector<double>>(i, std::move(outvalues)));
        // Memory is sometimes exhausted near here, so explicitly erase the vector.
        eigenvectors.erase(eigenvectors.end() - 1);
      }
    std::cout << "copied eigenvectors to PODBasis." << std::endl;
  }

  void reduced_matrix(dealii::SparseMatrix<double> &mass_matrix,
                      PODBasis &pod_basis, dealii::FullMatrix<double> &pod_mass_matrix)
  // Compute the reduced matrix from a given POD basis.
  {
    auto num_pod_vectors = pod_basis.get_num_pod_vectors();
    pod_mass_matrix.reinit(num_pod_vectors, num_pod_vectors);

    dealii::Vector<double> tmp(mass_matrix.m());
    for (unsigned int i = 0; i < num_pod_vectors; ++i)
      {
        for (unsigned int j = 0; j < num_pod_vectors; ++j)
          {
            mass_matrix.vmult(tmp, pod_basis.vectors[j]);
            pod_mass_matrix(i, j) = tmp * pod_basis.vectors[i];
          }
      }
  }
}
