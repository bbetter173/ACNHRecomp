#pragma once

namespace nx::nvn {

// Join the cache writer and save records from short sessions before an orderly process exit.
void pipeline_cache_shutdown();

}  // namespace nx::nvn
