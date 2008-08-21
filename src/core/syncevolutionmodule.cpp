#include <boost/python.hpp>
#include "scitbx/boost_python/container_conversions.h"

#include "SyncEvolutionCmdline.h"

#include <vector>
#include <string>

void f(std::string arg)
{
}

BOOST_PYTHON_MODULE(syncevolution)
{
    using namespace boost::python;

    class_<SyncEvolutionCmdline>("Cmdline", init<std::vector<string> >())
        .def("parse", &SyncEvolutionCmdline::parse)
        .def("run", &SyncEvolutionCmdline::run)
        ;

  
    scitbx::boost_python::container_conversions::from_python_sequence<
    std::vector<string>,
        scitbx::boost_python::container_conversions::variable_capacity_policy>();
}

// from http://cctbx.svn.sourceforge.net/viewvc/cctbx/trunk/scitbx/array_family/boost_python/regression_test_ext.cpp?view=markup
#if 0   
    boost::python::to_python_converter<
    boost::array<int, 2>, scitbx::boost_python::container_conversions::to_tuple<
    boost::array<int, 2> > >();

    scitbx::boost_python::container_conversions::from_python_sequence<
    std::list<double>,
        scitbx::boost_python::container_conversions::linked_list_policy>();
   
    scitbx::boost_python::container_conversions::from_python_sequence<
    boost::array<double, 3>,
        scitbx::boost_python::container_conversions::fixed_size_policy>();
   
    scitbx::boost_python::container_conversions::from_python_sequence<
    boost::array<double, 4>,
        scitbx::boost_python::container_conversions::fixed_size_policy>();

    scitbx::boost_python::container_conversions::from_python_sequence<
        af::small<double, 6>,
        scitbx::boost_python::container_conversions::fixed_capacity_policy>();
#endif
