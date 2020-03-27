#define pista_debug(...) if (_pista_log_level >= Debug) {fprintf(stderr, "[debug] " __VA_ARGS__); fflush(stderr);}
#define pista_info(...)  if (_pista_log_level >= Info ) {fprintf(stderr, "[info] "  __VA_ARGS__); fflush(stderr);}
#define pista_warn(...)  if (_pista_log_level >= Warn ) {fprintf(stderr, "[warn] "  __VA_ARGS__); fflush(stderr);}
#define pista_error(...) if (_pista_log_level >= Error) {fprintf(stderr, "[error] " __VA_ARGS__); fflush(stderr);}
#define pista_fatal(...)                                {fprintf(stderr, "[fatal] " __VA_ARGS__); exit(EXIT_FAILURE);}

typedef enum pista_LogLevel {
	Nothing,
	Error,
	Warn,
	Info,
	Debug
} LogLevel;

LogLevel _pista_log_level;
