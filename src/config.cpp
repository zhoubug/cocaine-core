#include "config.hpp"

using namespace cocaine;

const config_t& config_t::get() {
    return g_config;
}

config_t& config_t::set() {
    return g_config;
}

config_t config_t::g_config;
