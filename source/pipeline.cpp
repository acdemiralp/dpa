#include <dpa/pipeline.hpp>

#include <boost/mpi/environment.hpp>

#include <dpa/benchmark/benchmark.hpp>
#include <dpa/stages/argument_parser.hpp>
#include <dpa/stages/regular_grid_loader.hpp>
#include <dpa/stages/domain_partitioner.hpp>
#include <dpa/stages/integral_curve_saver.hpp>
#include <dpa/stages/particle_advector.hpp>
#include <dpa/stages/uniform_seed_generator.hpp>

namespace dpa
{
std::int32_t pipeline::run(std::int32_t argc, char** argv)
{
  boost::mpi::environment environment(argc, argv, boost::mpi::threading::level::serialized);
  std::cout << "Started pipeline on " << environment.processor_name() << "\n";

  auto arguments         = argument_parser::parse(argv[1]);
  auto benchmark_session = run_mpi<float, std::milli>([&] (session_recorder<float, std::milli>& recorder)
  {
    auto partitioner     = domain_partitioner ();
    auto loader          = regular_grid_loader(
      &partitioner                        , 
      arguments.input_dataset_filepath    , 
      arguments.input_dataset_name        , 
      arguments.input_dataset_spacing_name);
    auto advector        = particle_advector(
      &partitioner                                   , 
      arguments.particle_advector_particles_per_round,
      arguments.particle_advector_load_balancer      , 
      arguments.particle_advector_integrator         ,
      arguments.particle_advector_step_size          ,
      arguments.particle_advector_gather_particles   ,
      arguments.particle_advector_record             );

    auto vector_fields   = std::unordered_map<relative_direction, regular_vector_field_3d>();
    auto particles       = std::vector<particle<vector3, integer>>();

    std::cout << "1.domain_partitioning\n";
    recorder.record("1.domain_partitioning", [&] ()
    {
      partitioner.set_domain_size(loader.load_dimensions(), ivector3::Ones());
    });
    std::cout << "2.data_loading\n";
    recorder.record("2.data_loading"       , [&] ()
    {
      vector_fields = loader.load_vector_fields(
        arguments.particle_advector_load_balancer == "diffuse_constant"                       || 
        arguments.particle_advector_load_balancer == "diffuse_lesser_average"                 || 
        arguments.particle_advector_load_balancer == "diffuse_greater_limited_lesser_average" );
    });
    std::cout << "3.seed_generation\n";
    recorder.record("3.seed_generation"    , [&] ()
    {
      auto offset        = vector_fields[relative_direction::center].spacing.array() * partitioner.partitions().at(relative_direction::center).offset.cast<scalar>().array();
      auto size          = vector_fields[relative_direction::center].spacing.array() * partitioner.block_size()                                      .cast<scalar>().array();
      auto iterations    = arguments.seed_generation_iterations;
      auto process_index = partitioner.cartesian_communicator()->rank();
      auto boundaries    = arguments.seed_generation_boundaries ? arguments.seed_generation_boundaries : std::nullopt;

      if      (arguments.seed_generation_stride)
        particles = uniform_seed_generator::generate(
          offset       ,
          size         , 
          vector_fields[relative_direction::center].spacing.array() * arguments.seed_generation_stride->array(),
          iterations   , 
          process_index,
          boundaries   );
      else if (arguments.seed_generation_count)
        particles = uniform_seed_generator::generate_random(
          offset       ,
          size         , 
          *arguments.seed_generation_count,
          iterations   , 
          process_index,
          boundaries   );
      else if (arguments.seed_generation_range)
        particles = uniform_seed_generator::generate_random(
          offset       ,
          size         , 
          *arguments.seed_generation_range,
          iterations   , 
          process_index,
          boundaries   );
    });

    particle_advector::output output   = {};
    integer                   rounds   = 0;
    bool                      complete = false;
    while (!complete)
    {
      particle_advector::round_info round_info;
      std::cout << "4.1." + std::to_string(rounds) + ".load_balance_distribute\n";
      recorder.record("4.1." + std::to_string(rounds) + ".load_balance_distribute" , [&] ()
      {
                     advector.load_balance_distribute (               particles                                                      );
      });
      std::cout << "4.2." + std::to_string(rounds) + ".compute_round_info\n";
      recorder.record("4.2." + std::to_string(rounds) + ".compute_round_info"      , [&] ()
      {
        round_info = advector.compute_round_info      (               particles                                                      );
      });
      std::cout << "4.3." + std::to_string(rounds) + ".allocate_integral_curves\n";
      recorder.record("4.3." + std::to_string(rounds) + ".allocate_integral_curves", [&] ()
      {
                     advector.allocate_integral_curves(                                            output.integral_curves, round_info);
      });
      std::cout << "4.4." + std::to_string(rounds) + ".advect\n";
      recorder.record("4.4." + std::to_string(rounds) + ".advect"                  , [&] ()
      {
                     advector.advect                  (vector_fields, particles, output.particles, output.integral_curves, round_info);
      });
      std::cout << "4.5." + std::to_string(rounds) + ".load_balance_collect\n";
      recorder.record("4.5." + std::to_string(rounds) + ".load_balance_collect"    , [&] ()
      {
                     advector.load_balance_collect    (vector_fields,            output.particles,                         round_info);
      });
      std::cout << "4.6." + std::to_string(rounds) + ".out_of_bounds_distribute\n";
      recorder.record("4.6." + std::to_string(rounds) + ".out_of_bounds_distribute", [&] ()
      {
                     advector.out_of_bounds_distribute(               particles,                                           round_info);
      });
      std::cout << "4.7." + std::to_string(rounds) + ".check_completion\n";
      recorder.record("4.7." + std::to_string(rounds) + ".check_completion"        , [&] ()
      {
        complete =   advector.check_completion        (               particles                                                      );
      });  
      rounds++;
    }
    std::cout << "4.8.gather_particles\n";
    recorder.record("4.8.gather_particles"     , [&] ()
    {
      advector.gather_particles     (output.particles);
    });
    std::cout << "4.9.prune_integral_curves\n";
    recorder.record("4.9.prune_integral_curves", [&] ()
    {
      advector.prune_integral_curves(output.integral_curves);
    });

    std::cout << "5.data_saving\n";
    recorder.record("5.data_saving"            , [&] ()
    {
      if (arguments.particle_advector_record)
        integral_curve_saver(&partitioner, arguments.output_dataset_filepath).save_integral_curves(output.integral_curves, (std::size_t(arguments.particle_advector_particles_per_round) * arguments.seed_generation_iterations) > std::numeric_limits<std::uint32_t>::max());
    });
  }, 10);
  benchmark_session.gather();
  benchmark_session.to_csv(arguments.output_dataset_filepath + ".benchmark.csv");
  return 0;
}
}
