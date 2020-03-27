#define khlib_debug(...) if (_khlib_log_level >= Debug) {fprintf(stderr, "[debug] " __VA_ARGS__); fflush(stderr);}
#define khlib_info(...)  if (_khlib_log_level >= Info ) {fprintf(stderr, "[info] "  __VA_ARGS__); fflush(stderr);}
#define khlib_warn(...)  if (_khlib_log_level >= Warn ) {fprintf(stderr, "[warn] "  __VA_ARGS__); fflush(stderr);}
#define khlib_error(...) if (_khlib_log_level >= Error) {fprintf(stderr, "[error] " __VA_ARGS__); fflush(stderr);}
#define khlib_fatal(...)                                {fprintf(stderr, "[fatal] " __VA_ARGS__); exit(EXIT_FAILURE);}

typedef enum khlib_LogLevel {
	Nothing,
	Error,
	Warn,
	Info,
	Debug
} LogLevel;

LogLevel _khlib_log_level;
