#include <catch2/catch_test_macros.hpp>
#include <catch2/internal/catch_clara.hpp>
#include <catch2/catch_session.hpp>
#include <string>

#include <hpc/compiler.h>
#include <hpc/log.h>

static auto opt_log_verb = 0;
static auto opt_log_file = std::string{};

void
enableLogging(void)
{
	auto abc = +[](char *msg, unsigned int x) { UNSCOPED_INFO("" << msg); };
	log_verbose = opt_log_verb;
	log_setcaps(31);

	if (opt_log_file.empty())
		log_set_handler(abc);
	else
		log_open(opt_log_file.c_str());
}

void
disableLogging(void)
{
	auto abc = +[](char *msg, unsigned int x) { };
	log_verbose=0;
	log_setcaps(0);
	log_set_handler(abc);
}

int
main(int argc, char* argv[])
{
	Catch::Session session;
	using namespace Catch::Clara;
	auto cli = session.cli() |
	  Opt(opt_log_verb, "log-verbose")["-g"]["--log-verbose"]("verbosity") |
	  Opt(opt_log_file, "log-file")["-F"]["--log-file"]("log file");

	session.cli(cli); 
	int rv = session.applyCommandLine(argc,argv);
	if( rv != 0 )
		return rv;
	//enableLogging();
	return session.run();
}
