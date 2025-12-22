#pragma once

#include <boost/dll.hpp>

#include "ModuleInterface.h"

struct LibInfo {
  MetaData metaData;
  boost::dll::shared_library lib;
};

class ModelManager {
};