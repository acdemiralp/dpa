#include <dpa/stages/domain_partitioner.hpp>

#include <dpa/math/prime_factorization.hpp>

namespace dpa
{
void                                                                         domain_partitioner::set_domain_size       (const ivector3& domain_size, const ivector3& ghost_cell_size)
{
  domain_size_     = domain_size;
  ghost_cell_size_ = ghost_cell_size;

  auto prime_factors = prime_factorize(communicator_.size());
  auto current_size  = domain_size_;
  grid_size_.setConstant(1);
  while (!prime_factors.empty())
  {
    auto dimension = 0;
    for (auto i = 0; i < 3; ++i)
      if (current_size[i] > current_size[dimension])
        dimension = i;

    current_size[dimension] /= prime_factors.back();
    grid_size_  [dimension] *= prime_factors.back();
    prime_factors.pop_back();
  }
  
  block_size_             = domain_size_.array() / grid_size_.array();
  cartesian_communicator_ = std::make_unique<boost::mpi::cartesian_communicator>(communicator_, boost::mpi::cartesian_topology(std::vector<boost::mpi::cartesian_dimension>
  {
    boost::mpi::cartesian_dimension(grid_size_[0]),
    boost::mpi::cartesian_dimension(grid_size_[1]),
    boost::mpi::cartesian_dimension(grid_size_[2])
  }));

  partitions_.clear();
  partitions_[relative_direction::center] = setup_partition(cartesian_communicator_->rank());

  const auto shift_x = cartesian_communicator_->shifted_ranks(0, 1);
  const auto shift_y = cartesian_communicator_->shifted_ranks(1, 1);
  const auto shift_z = cartesian_communicator_->shifted_ranks(2, 1);
  if (shift_x.first  != MPI_PROC_NULL) partitions_[relative_direction::negative_x] = setup_partition(shift_x.first );
  if (shift_x.second != MPI_PROC_NULL) partitions_[relative_direction::positive_x] = setup_partition(shift_x.second);
  if (shift_y.first  != MPI_PROC_NULL) partitions_[relative_direction::negative_y] = setup_partition(shift_y.first );
  if (shift_y.second != MPI_PROC_NULL) partitions_[relative_direction::positive_y] = setup_partition(shift_y.second);
  if (shift_z.first  != MPI_PROC_NULL) partitions_[relative_direction::negative_z] = setup_partition(shift_z.first );
  if (shift_z.second != MPI_PROC_NULL) partitions_[relative_direction::positive_z] = setup_partition(shift_z.second);
}

boost::mpi::communicator*                                                    domain_partitioner::communicator          ()
{                                    
  return &communicator_;              
}
boost::mpi::cartesian_communicator*                                          domain_partitioner::cartesian_communicator()
{
  return cartesian_communicator_.get();
}
const ivector3&                                                              domain_partitioner::domain_size           () const
{                                    
  return domain_size_;               
}                                    
const ivector3&                                                              domain_partitioner::grid_size             () const
{                                    
  return grid_size_;                 
}                                    
const ivector3&                                                              domain_partitioner::block_size            () const
{                                    
  return block_size_;
}
const std::unordered_map<relative_direction, domain_partitioner::partition>& domain_partitioner::partitions            () const
{
  return partitions_;
}

std::string                                                                  domain_partitioner::to_string             () const
{
  std::stringstream stream;
  stream << "Rank            " << cartesian_communicator_->rank() << "\n";
  stream << "Domain size     " << domain_size_    [0] << " " << domain_size_    [1] << " " << domain_size_    [2] << "\n";
  stream << "Ghost cell size " << ghost_cell_size_[0] << " " << ghost_cell_size_[1] << " " << ghost_cell_size_[2] << "\n";
  stream << "Grid size       " << grid_size_      [0] << " " << grid_size_      [1] << " " << grid_size_      [2] << "\n";
  stream << "Block size      " << block_size_     [0] << " " << block_size_     [1] << " " << block_size_     [2] << "\n";
  stream << "Partitions      " << "\n";
  for (auto& partition : partitions_)
  {
    stream << "  Partition            " << std::uint32_t(partition.first) << "\n";
    stream << "    Rank               " << partition.second.rank          << "\n";
    stream << "    Multi rank         " << partition.second.multi_rank        [0] << " " << partition.second.multi_rank        [1] << " " << partition.second.multi_rank        [2] << "\n";
    stream << "    Offset             " << partition.second.offset            [0] << " " << partition.second.offset            [1] << " " << partition.second.offset            [2] << "\n";
    stream << "    Ghosted offset     " << partition.second.ghosted_offset    [0] << " " << partition.second.ghosted_offset    [1] << " " << partition.second.ghosted_offset    [2] << "\n";
    stream << "    Ghosted block size " << partition.second.ghosted_block_size[0] << " " << partition.second.ghosted_block_size[1] << " " << partition.second.ghosted_block_size[2] << "\n";
  }
  return stream.str();
}

domain_partitioner::partition                                                domain_partitioner::setup_partition       (integer rank) const
{
  const auto raw_multi_rank = cartesian_communicator_->coordinates(rank);
  const auto multi_rank     = ivector3(raw_multi_rank[0], raw_multi_rank[1], raw_multi_rank[2]);
  const auto offset         = block_size_.array() * multi_rank.array();

  auto ghosted_offset = ivector3();
  auto ghosted_size   = ivector3();
  for (auto i = 0; i < 3; ++i)
  {
    if (offset[i] >= ghost_cell_size_ [i])
      ghosted_offset[i] = offset[i] - ghost_cell_size_[i];
    else
      ghosted_offset[i] = 0;
    
    if (offset[i] + block_size_[i] + ghost_cell_size_[i] < domain_size_[i])
      ghosted_size  [i] = block_size_ [i] + ghost_cell_size_[i];
    else
      ghosted_size  [i] = domain_size_[i] - offset[i];
  }

  return partition {rank, multi_rank, offset, ghosted_offset, ghosted_size};
}
}
