engine_add_module(jobs)
target_sources(engine_jobs PRIVATE src/jobs/JobExecutor.cpp src/jobs/BackgroundJobQueue.cpp)
find_package(Threads REQUIRED)
target_link_libraries(engine_jobs PRIVATE mir::core_base Threads::Threads)
