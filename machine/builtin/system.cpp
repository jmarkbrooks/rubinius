#include "arguments.hpp"
#include "builtin/array.hpp"
#include "builtin/bignum.hpp"
#include "builtin/block_environment.hpp"
#include "builtin/channel.hpp"
#include "builtin/class.hpp"
#include "builtin/compact_lookup_table.hpp"
#include "builtin/constant_scope.hpp"
#include "builtin/exception.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/float.hpp"
#include "builtin/io.hpp"
#include "builtin/location.hpp"
#include "builtin/lookup_table.hpp"
#include "builtin/method_table.hpp"
#include "builtin/thread.hpp"
#include "builtin/tuple.hpp"
#include "builtin/string.hpp"
#include "builtin/symbol.hpp"
#include "builtin/system.hpp"
#include "builtin/variable_scope.hpp"

#include "call_frame.hpp"
#include "compiled_file.hpp"
#include "configuration.hpp"
#include "config_parser.hpp"
#include "dtrace/dtrace.h"
#include "environment.hpp"
#include "memory/walker.hpp"
#include "global_cache.hpp"
#include "helpers.hpp"
#include "instruments/tooling.hpp"
#include "lookup_data.hpp"
#include "memory.hpp"
#include "object_utils.hpp"
#include "on_stack.hpp"
#include "signal.hpp"
#include "thread_phase.hpp"
#include "windows_compat.h"

#include "util/sha1.h"
#include "util/timing.h"
#include "logger.hpp"

#include "paths.h"

#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <stdarg.h>
#include <string.h>
#include <sstream>
#include <signal.h>
#include <unistd.h>

#ifndef RBX_WINDOWS
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <dlfcn.h>
#endif

#include "jit/llvm/state.hpp"
#include "jit/llvm/context.hpp"
#include "jit/llvm/compiler.hpp"

#include "missing/setproctitle.h"

namespace rubinius {

  void System::bootstrap_methods(STATE) {
    System::attach_primitive(state,
                             G(rubinius), true,
                             state->symbol("open_class"),
                             state->symbol("vm_open_class"));

    System::attach_primitive(state,
                             G(rubinius), true,
                             state->symbol("open_class_under"),
                             state->symbol("vm_open_class_under"));

    System::attach_primitive(state,
                             G(rubinius), true,
                             state->symbol("open_module"),
                             state->symbol("vm_open_module"));

    System::attach_primitive(state,
                             G(rubinius), true,
                             state->symbol("open_module_under"),
                             state->symbol("vm_open_module_under"));

    System::attach_primitive(state,
                             G(rubinius), true,
                             state->symbol("add_defn_method"),
                             state->symbol("vm_add_method"));

    System::attach_primitive(state,
                             G(rubinius), true,
                             state->symbol("attach_method"),
                             state->symbol("vm_attach_method"));

    System::attach_primitive(state,
                             as<Module>(G(rubinius)->get_const(state, "Type")), true,
                             state->symbol("object_singleton_class"),
                             state->symbol("vm_object_singleton_class"));

  }

  void System::attach_primitive(STATE, Module* mod, bool meta,
                                Symbol* name, Symbol* prim)
  {
    MethodTable* tbl;

    if(meta) {
      tbl = mod->singleton_class(state)->method_table();
    } else {
      tbl = mod->method_table();
    }

    Executable* oc = Executable::allocate(state, cNil);
    oc->primitive(state, prim);
    oc->resolve_primitive(state);

    tbl->store(state, name, nil<String>(), oc, nil<ConstantScope>(),
        Fixnum::from(0), G(sym_public));
  }

/* Primitives */
  //
  // HACK: remove this when performance is better and compiled_file.rb
  // unmarshal_data method works.
  Object* System::compiledfile_load(STATE, String* path,
                                    Integer* signature, Integer* version)
  {
    std::ifstream stream(path->c_str(state));
    if(!stream) {
      return Primitives::failure();
    }

    CompiledFile* cf = CompiledFile::load(stream);
    if(cf->magic != "!RBIX") {
      delete cf;
      return Primitives::failure();
    }

    uint64_t sig = signature->to_ulong_long();
    if((sig > 0 && cf->signature != sig)) {
      delete cf;
      return Primitives::failure();
    }

    Object *body = cf->body(state);

    delete cf;
    return body;
  }

  Object* System::yield_gdb(STATE, Object* obj) {
    obj->show(state);
    Exception::raise_assertion_error(state, "yield_gdb called and not caught");
    return obj;
  }

  void exec_sh_fallback(STATE, const char* c_str, size_t c_len) {
    char* s = const_cast<char*>(c_str);
    bool use_sh = false;

    for(;*s;s++) {
      if(*s != ' ' && !ISALPHA(*s) && strchr("*?{}[]<>()~&|\\$;'`\"\n\t\r\f\v",*s)) {
        use_sh = true;
        break;
      }
    }

    if(use_sh) {
      execl("/bin/sh", "sh", "-c", c_str, (char*)0);
    } else {
      size_t max_spaces = (c_len / 2) + 2;
      char** args = new char*[max_spaces];

      // Now put nulls for spaces into c_str and assign each bit
      // to args to create the array of char*s that execv wants.

      s = const_cast<char*>(c_str);
      const char* s_end = c_str + c_len;
      int idx = 0;

      for(;;) {
        // turn the next group of spaces into nulls.
        while(s < s_end && *s == ' ') {
          *s = 0;
          s++;
        }

        // Hit the end, bail.
        if(s == s_end) break;

        // Write the address of the next chunk here.
        args[idx++] = s;

        // Skip to the next space
        while(s < s_end && *s != ' ') s++;
      }

      args[idx] = 0;

      // If we added anything, then exec, otherwise fall through and fail.
      if(idx > 0) execvp(args[0], args);
      // If we failed, clean up the args.
      delete[] args;
    }
  }

  class ExecCommand {
    char* command_;
    size_t command_size_;
    size_t argc_;
    char** argv_;

    char* make_string(STATE, String* source) {
      const char* src = source->c_str_null_safe(state);
      size_t len = strnlen(src, source->byte_size());
      char* str = new char[len + 1];

      memcpy(str, src, len);
      str[len] = 0;

      return str;
    }

  public:
    ExecCommand(STATE, String* command)
      : argc_(0)
      , argv_(NULL)
    {
      command_ = make_string(state, command);
      command_size_ = command->byte_size();
    }

    ExecCommand(STATE, String* command, Array* args) {
      command_ = make_string(state, command);
      command_size_ = command->byte_size();

      argc_ = args->size();
      argv_ = NULL;

      if(argc_ > 0) {
        argv_ = new char*[argc_ + 1];

        /* execvp() requires a NULL as last element */
        argv_[argc_] = NULL;

        for(size_t i = 0; i < argc_; i++) {
          /* POSIX guarantees that execvp does not modify the characters to
           * which the argv pointers point, despite the argument not being
           * declared as const char *const[].
           */
          argv_[i] = make_string(state, as<String>(args->get(state, i)));
        }
      }
    }

    ~ExecCommand() {
      if(argc_ > 0 && argv_) {
        for(size_t i = 0; i < argc_; i++) {
          delete[] argv_[i];
        }

        delete[] argv_;
      }

      delete[] command_;
    }

    char* command() {
      return command_;
    }

    size_t command_size() {
      return command_size_;
    }

    size_t argc() {
      return argc_;
    }

    char** argv() {
      return argv_;
    }
  };

  static void redirect_file_descriptor(int from, int to) {
    if(dup2(to, from) < 0) return;

    int flags = fcntl(from, F_GETFD);
    if(flags < 0) return;

    fcntl(from, F_SETFD, flags & ~FD_CLOEXEC);
  }

  Object* System::vm_spawn_setup(STATE, Object* spawn_state) {
    if(LookupTable* table = try_as<LookupTable>(spawn_state)) {
      if(Array* env = try_as<Array>(
            table->fetch(state, state->symbol("env"))))
      {
        native_int size = env->size();
        for(native_int i = 0; i < size; i += 2) {
          const char* key = as<String>(env->get(state, i))->c_str_null_safe(state);
          Object* value = env->get(state, i + 1);

          if(value->nil_p()) {
            unsetenv(key);
          } else {
            setenv(key, as<String>(value)->c_str_null_safe(state), 1);
          }
        }
      }

      if(Fixnum* pgrp = try_as<Fixnum>(
            table->fetch(state, state->symbol("pgroup"))))
      {
        setpgid(0, pgrp->to_native());
      }

      if(Fixnum* mask = try_as<Fixnum>(
            table->fetch(state, state->symbol("umask"))))
      {
        umask(mask->to_native());
      }

      if(String* str = try_as<String>(
            table->fetch(state, state->symbol("chdir"))))
      {
        const char* dir = str->c_str_null_safe(state);
        if(chdir(dir) < 0) {
          logger::error("%s: spawn: failed to change directory: %s",
              strerror(errno), dir);
        }
      }

      if(CBOOL(table->has_key(state, state->symbol("close_others")))) {
        int max = IO::max_descriptors();
        int flags;

        for(int fd = STDERR_FILENO + 1; fd < max; fd++) {
          if((flags = fcntl(fd, F_GETFD)) >= 0) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
          }
        }
      }

      if(Array* assign = try_as<Array>(
            table->fetch(state, state->symbol("assign_fd"))))
      {
        native_int size = assign->size();
        for(native_int i = 0; i < size; i += 4) {
          int from = as<Fixnum>(assign->get(state, i))->to_native();
          int mode = as<Fixnum>(assign->get(state, i + 2))->to_native();
          int perm = as<Fixnum>(assign->get(state, i + 3))->to_native();
          const char* name = as<String>(assign->get(state, i + 1))->c_str_null_safe(state);

          int to = IO::open_with_cloexec(state, name, mode, perm);
          redirect_file_descriptor(from, to);
        }
      }

      if(Array* redirect = try_as<Array>(
            table->fetch(state, state->symbol("redirect_fd"))))
      {
        native_int size = redirect->size();
        for(native_int i = 0; i < size; i += 2) {
          int from = as<Fixnum>(redirect->get(state, i))->to_native();
          int to = as<Fixnum>(redirect->get(state, i + 1))->to_native();

          redirect_file_descriptor(from, to < 0 ? (-to + 1) : to);
        }
      }
    }

    return cNil;
  }

  static int fork_exec(STATE, int errors_fd) {
    utilities::thread::SpinLock::LockGuard guard(state->shared().env()->fork_exec_lock());

    state->shared().internal_threads()->before_fork_exec(state);

    // If execvp() succeeds, we'll read EOF and know.
    fcntl(errors_fd, F_SETFD, FD_CLOEXEC);

    int pid;

    state->vm()->become_managed();

    {
      LockPhase locked(state);

      pid = ::fork();

      if(pid == 0) state->vm()->after_fork_child(state);
    }

    state->vm()->become_unmanaged();

    if(pid > 0) {
      state->shared().internal_threads()->after_fork_exec_parent(state);
    }

    return pid;
  }

  Object* System::vm_spawn(STATE, Object* spawn_state, String* path,
                           Array* args)
  {
    OnStack<1> os(state, spawn_state);

    /* Setting up the command and arguments may raise an exception so do it
     * before everything else.
     */
    ExecCommand exe(state, path, args);

    int errors[2];

    if(pipe(errors) != 0) {
      Exception::raise_errno_error(state, "error setting up pipes", errno, "pipe(2)");
      return NULL;
    }

    int pid;

    {
      UnmanagedPhase unmanaged(state);

      pid = fork_exec(state, errors[1]);
    }

    // error
    if(pid == -1) {
      close(errors[0]);
      close(errors[1]);

      Exception::raise_errno_error(state, "error forking", errno, "fork(2)");
      return NULL;
    }

    if(pid == 0) {
      close(errors[0]);

      state->vm()->thread->init_lock();
      state->shared().internal_threads()->after_fork_exec_child(state);

      // Setup ENV, redirects, groups, etc. in the child before exec().
      vm_spawn_setup(state, spawn_state);

      /* Reset all signal handlers to the defaults, so any we setup in
       * Rubinius won't leak through. We need to use sigaction() here since
       * signal() provides no control over SA_RESTART and can use the wrong
       * value causing blocking I/O methods to become uninterruptable.
       */
      for(int i = 1; i < NSIG; i++) {
        struct sigaction action;

        action.sa_handler = SIG_DFL;
        action.sa_flags = 0;
        sigfillset(&action.sa_mask);

        sigaction(i, &action, NULL);
      }

      if(exe.argc()) {
        (void)::execvp(exe.command(), exe.argv());
      } else {
        exec_sh_fallback(state, exe.command(), exe.command_size());
      }

      /* execvp() returning means it failed. */
      std::ostringstream command_line;
      command_line << exe.command();
      for(size_t i = 0; i < exe.argc(); i++) {
        command_line << " " << exe.argv()[i];
      }
      logger::error("%s: spawn: exec failed: %s",
          strerror(errno), command_line.str().c_str());

      int error_no = errno;
      if(write(errors[1], &error_no, sizeof(int)) < 0) {
        logger::error("%s: spawn: writing error status", strerror(errno));
      }
      close(errors[1]);

      exit(1);
    }

    close(errors[1]);

    CallFrame* call_frame = state->vm()->get_ruby_frame(3);

    logger::write("spawn: %d: %s, %s, %s:%d",
        pid, exe.command(),
        state->vm()->name().c_str(),
        call_frame->file(state)->cpp_str(state).c_str(),
        call_frame->line(state));

    int error_no;
    ssize_t size;

    while((size = read(errors[0], &error_no, sizeof(int))) < 0) {
      switch(errno) {
      case EAGAIN:
      case EINTR:
        continue;
      default:
        logger::error("%s: spawn: reading error status", strerror(errno));
        break;
      }
    }
    close(errors[0]);

    if(size != 0) {
      Exception::raise_errno_error(state, "execvp(2) failed", error_no);
      return NULL;
    }

    return Fixnum::from(pid);
  }

  Object* System::vm_backtick(STATE, String* str) {
#ifdef RBX_WINDOWS
    // TODO: Windows
    return Primitives::failure();
#else
    /* Setting up the command may raise an exception so do it before
     * everything else.
     */
    ExecCommand exe(state, str);

    int errors[2], output[2];

    if(pipe(errors) != 0) {
      Exception::raise_errno_error(state, "error setting up pipes", errno, "pipe(2)");
      return NULL;
    }

    if(pipe(output) != 0) {
      Exception::raise_errno_error(state, "error setting up pipes", errno, "pipe(2)");
      return NULL;
    }

    int pid;

    {
      UnmanagedPhase unmanaged(state);

      pid = fork_exec(state, errors[1]);
    }

    // error
    if(pid == -1) {
      close(errors[0]);
      close(errors[1]);
      close(output[0]);
      close(output[1]);

      Exception::raise_errno_error(state, "error forking", errno, "fork(2)");
      return NULL;
    }

    if(pid == 0) {
      state->vm()->thread->init_lock();
      state->shared().internal_threads()->after_fork_exec_child(state);

      close(errors[0]);
      close(output[0]);

      dup2(output[1], STDOUT_FILENO);
      close(output[1]);

      /* Reset all signal handlers to the defaults, so any we setup in
       * Rubinius won't leak through. We need to use sigaction() here since
       * signal() provides no control over SA_RESTART and can use the wrong
       * value causing blocking I/O methods to become uninterruptable.
       */
      for(int i = 1; i < NSIG; i++) {
        struct sigaction action;

        action.sa_handler = SIG_DFL;
        action.sa_flags = 0;
        sigfillset(&action.sa_mask);

        sigaction(i, &action, NULL);
      }

      exec_sh_fallback(state, exe.command(), exe.command_size());

      /* execvp() returning means it failed. */
      logger::error("%s: backtick: exec failed: %s",
          strerror(errno), exe.command());

      int error_no = errno;
      if(write(errors[1], &error_no, sizeof(int)) < 0) {
        logger::error("%s: backtick: writing error status", strerror(errno));
      }
      close(errors[1]);

      exit(1);
    }

    close(errors[1]);
    close(output[1]);

    CallFrame* call_frame = state->vm()->get_ruby_frame(1);

    logger::write("backtick: %d: %s, %s, %s:%d",
        pid, exe.command(),
        state->vm()->name().c_str(),
        call_frame->file(state)->cpp_str(state).c_str(),
        call_frame->line(state));

    int error_no;
    ssize_t size;

    while((size = read(errors[0], &error_no, sizeof(int))) < 0) {
      switch(errno) {
      case EAGAIN:
      case EINTR:
        continue;
      default:
        logger::error("%s: backtick: reading error status", strerror(errno));
        break;
      }
    }
    close(errors[0]);

    if(size != 0) {
      close(output[0]);
      Exception::raise_errno_error(state, "execvp(2) failed", error_no);
      return NULL;
    }

    std::string buf;
    for(;;) {

      ssize_t bytes = 0;
      char raw_buf[1024];
      {
        UnmanagedPhase unmanaged(state);
        bytes = read(output[0], raw_buf, 1023);
      }

      if(bytes < 0) {
        switch(errno) {
          case EAGAIN:
          case EINTR:
            if(!state->check_async(state)) {
              close(output[0]);
              return NULL;
            }
            continue;
          default:
            close(output[0]);
            Exception::raise_errno_error(state, "reading child data", errno, "read(2)");
        }
      }

      if(bytes == 0) {
        break;
      }
      buf.append(raw_buf, bytes);
    }

    close(output[0]);

    return Tuple::from(state, 2, Fixnum::from(pid),
                       String::create(state, buf.c_str(), buf.size()));
#endif  // RBX_WINDOWS
  }

  Object* System::vm_exec(STATE, String* path, Array* args) {
    /* Setting up the command and arguments may raise an exception so do it
     * before everything else.
     */
    ExecCommand exe(state, path, args);

    CallFrame* call_frame = state->vm()->get_ruby_frame(3);

    logger::write("exec: %s, %s, %s:%d", exe.command(),
        state->vm()->name().c_str(),
        call_frame->file(state)->cpp_str(state).c_str(),
        call_frame->line(state));

    // From this point, we are serialized.
    utilities::thread::SpinLock::LockGuard guard(state->shared().env()->fork_exec_lock());

    state->shared().internal_threads()->before_exec(state);

    void* old_handlers[NSIG];

    /* Reset all signal handlers to the defaults, so any we setup in Rubinius
     * won't leak through. We need to use sigaction() here since signal()
     * provides no control over SA_RESTART and can use the wrong value causing
     * blocking I/O methods to become uninterruptable.
     */
    for(int i = 1; i < NSIG; i++) {
      struct sigaction action;
      struct sigaction old_action;

      action.sa_handler = SIG_DFL;
      action.sa_flags = 0;
      sigfillset(&action.sa_mask);

      sigaction(i, &action, &old_action);
      old_handlers[i] = (void*)old_action.sa_handler;
    }

    if(exe.argc()) {
      (void)::execvp(exe.command(), exe.argv());
    } else {
      exec_sh_fallback(state, exe.command(), exe.command_size());
    }

    int erno = errno;

    // Hmmm, execvp failed, we need to recover here.

    for(int i = 1; i < NSIG; i++) {
      struct sigaction action;

      action.sa_handler = (void(*)(int))old_handlers[i];
      action.sa_flags = 0;
      sigfillset(&action.sa_mask);

      sigaction(i, &action, NULL);
    }

    state->shared().internal_threads()->after_exec(state);

    /* execvp() returning means it failed. */
    Exception::raise_errno_error(state, "execvp(2) failed", erno);
    return NULL;
  }

  Object* System::vm_wait_pid(STATE, Fixnum* pid_obj, Object* no_hang) {
#ifdef RBX_WINDOWS
    // TODO: Windows
    return Primitives::failure();
#else
    pid_t input_pid = pid_obj->to_native();
    int options = 0;
    int status;
    pid_t pid;

    if(CBOOL(no_hang)) {
      options |= WNOHANG;
    }

    typedef void (*rbx_sighandler_t)(int);

    rbx_sighandler_t hup_func;
    rbx_sighandler_t quit_func;
    rbx_sighandler_t int_func;

  retry:

    hup_func  = signal(SIGHUP, SIG_IGN);
    quit_func = signal(SIGQUIT, SIG_IGN);
    int_func  = signal(SIGINT, SIG_IGN);

    {
      UnmanagedPhase unmanaged(state);
      pid = waitpid(input_pid, &status, options);
    }

    signal(SIGHUP, hup_func);
    signal(SIGQUIT, quit_func);
    signal(SIGINT, int_func);

    if(pid == -1) {
      if(errno == ECHILD) return cFalse;
      if(errno == EINTR) {
        if(!state->check_async(state)) return NULL;
        goto retry;
      }

      // TODO handle other errnos?
      return cFalse;
    }

    if(CBOOL(no_hang) && pid == 0) {
      return cNil;
    }

    Object* output  = cNil;
    Object* termsig = cNil;
    Object* stopsig = cNil;

    if(WIFEXITED(status)) {
      output = Fixnum::from(WEXITSTATUS(status));
    } else if(WIFSIGNALED(status)) {
      termsig = Fixnum::from(WTERMSIG(status));
    } else if(WIFSTOPPED(status)){
      stopsig = Fixnum::from(WSTOPSIG(status));
    }

    return Tuple::from(state, 4, output, termsig, stopsig, Fixnum::from(pid));
#endif  // RBX_WINDOWS
  }

  Object* System::vm_exit(STATE, Fixnum* code) {
    state->vm()->thread_state()->raise_exit(code);
    return NULL;
  }

  Fixnum* System::vm_fork(STATE)
  {
#ifdef RBX_WINDOWS
    // TODO: Windows
    return force_as<Fixnum>(Primitives::failure());
#else
    int pid = -1;

    {
      utilities::thread::SpinLock::LockGuard guard(state->shared().env()->fork_exec_lock());

      state->shared().internal_threads()->before_fork(state);

      LockPhase locked(state);

      pid = ::fork();

      if(pid == 0) {
        state->vm()->after_fork_child(state);
      } else if(pid > 0) {
        state->shared().internal_threads()->after_fork_parent(state);
      }
    }

    // We're in the parent...
    if(pid > 0) {
      CallFrame* call_frame = state->vm()->get_ruby_frame(2);

      logger::write("fork: child: %d, %s, %s:%d", pid,
          state->vm()->name().c_str(),
          call_frame->file(state)->cpp_str(state).c_str(),
          call_frame->line(state));
    }

    // We're in the child...
    if(pid == 0) {
      /*  @todo any other re-initialisation needed? */

      state->vm()->thread->init_lock();
      state->shared().after_fork_child(state);
      state->shared().internal_threads()->after_fork_child(state);

      // In the child, the PID is nil in Ruby.
      return nil<Fixnum>();
    }

    if(pid == -1) {
      Exception::raise_errno_error(state, "fork(2) failed");
      return NULL;
    }

    return Fixnum::from(pid);
#endif
  }

  Object* System::vm_gc_start(STATE, Object* force) {
    // force is set if this is being called by the core library (for instance
    // in File#ininitialize). If we decided to ignore some GC.start calls
    // by usercode trying to be clever, we can use force to know that we
    // should NOT ignore it.
    if(CBOOL(force) || state->shared().config.gc_honor_start) {
      state->memory()->collect(state);
    }
    return cNil;
  }

  Object* System::vm_get_config_item(STATE, String* var) {
    ConfigParser::Entry* ent = state->shared().user_variables.find(var->c_str(state));
    if(!ent) return cNil;

    if(ent->is_number()) {
      return Integer::from_cppstr(state, ent->value, 10);
    } else if(ent->is_true()) {
      return cTrue;
    }

    return String::create(state, ent->value.c_str(), ent->value.size());
  }

  Object* System::vm_get_config_section(STATE, String* section) {
    ConfigParser::EntryList* list;

    list = state->shared().user_variables.get_section(
        reinterpret_cast<char*>(section->byte_address()));

    Array* ary = Array::create(state, list->size());
    for(size_t i = 0; i < list->size(); i++) {
      std::string variable = list->at(i)->variable;
      std::string value    = list->at(i)->value;
      String* var = String::create(state, variable.c_str(), variable.size());
      String* val = String::create(state, value.c_str(), value.size());

      ary->set(state, i, Tuple::from(state, 2, var, val));
    }

    delete list;

    return ary;
  }

  Object* System::vm_reset_method_cache(STATE, Module* mod, Symbol* name) {

    if(!state->vm()->global_cache()->has_seen(state, name)) return cTrue;

    state->vm()->global_cache()->clear(state, name);
    mod->reset_method_cache(state, name);

    state->vm()->metrics().machine.inline_cache_resets++;

    if(state->shared().config.ic_debug) {
      String* mod_name = mod->get_name(state);

      if(mod_name->nil_p()) {
        mod_name = String::create(state, "<unknown>");
      }

      std::cerr << std::endl
                << "reset global/method cache for "
                << mod_name->c_str(state)
                << "#"
                << name->debug_str(state).c_str()
                << std::endl;

      if(CallFrame* frame = state->vm()->get_ruby_frame(1)) {
        frame->print_backtrace(state, std::cerr, 6, true);
      }
    }

    return cTrue;
  }

   /*  @todo Could possibly capture the system backtrace at this
   *        point. --rue
   */
  Array* System::vm_backtrace(STATE, Fixnum* skip) {
    return Location::from_call_stack(state, skip->to_native());
  }

  Array* System::vm_mri_backtrace(STATE, Fixnum* skip) {
    return Location::mri_backtrace(state, skip->to_native());
  }


  Object* System::vm_show_backtrace(STATE) {
    state->vm()->call_frame()->print_backtrace(state);
    return cNil;
  }

  Object* System::vm_tooling_available_p(STATE) {
#ifdef RBX_PROFILER
    return RBOOL(state->shared().tool_broker()->available(state));
#else
    return cFalse;
#endif
  }

  Object* System::vm_tooling_active_p(STATE) {
    return RBOOL(state->vm()->tooling());
  }

  Object* System::vm_tooling_enable(STATE) {
    state->shared().tool_broker()->enable(state);
    return cTrue;
  }

  Object* System::vm_tooling_disable(STATE) {
    return state->shared().tool_broker()->results(state);
  }

  Object* System::vm_load_tool(STATE, String* str) {
    std::string path = std::string(str->c_str(state)) + ".";

#ifdef _WIN32
    path += "dll";
#else
  #ifdef __APPLE_CC__
    path += "bundle";
  #else
    path += "so";
  #endif
#endif

    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if(!handle) {
      path = std::string(RBX_LIB_PATH) + "/" + path;

      handle = dlopen(path.c_str(), RTLD_NOW);
      if(!handle) {
        return Tuple::from(state, 2, cFalse, String::create(state, dlerror()));
      }
    }

    void* sym = dlsym(handle, "Tool_Init");
    if(!sym) {
      dlclose(handle);
      return Tuple::from(state, 2, cFalse, String::create(state, dlerror()));
    } else {
      typedef int (*init_func)(rbxti::Env* env);
      init_func init = (init_func)sym;

      if(!init(state->vm()->tooling_env())) {
        dlclose(handle);
        return Tuple::from(state, 2, cFalse, String::create(state, path.c_str(), path.size()));
      }
    }

    return Tuple::from(state, 1, cTrue);
  }

  Object* System::vm_write_error(STATE, String* str) {
    std::cerr << str->c_str(state) << std::endl;
    return cNil;
  }

  Object* System::vm_watch_signal(STATE, Fixnum* sig, Object* ignored) {
    SignalThread* st = state->shared().signals();

    if(st) {
      native_int i = sig->to_native();
      if(i < 0) {
        st->add_signal_handler(state, -i, SignalThread::eDefault);
      } else if(i > 0) {
        st->add_signal_handler(state, i,
            CBOOL(ignored) ? SignalThread::eIgnore : SignalThread::eCustom);
      }

      return cTrue;
    } else {
      return cFalse;
    }
  }

  Object* System::vm_time(STATE) {
    return Integer::from(state, time(0));
  }

#define NANOSECONDS 1000000000
  Object* System::vm_sleep(STATE, Object* duration) {
    struct timespec ts = {0,0};
    bool use_timed_wait = true;

    if(Fixnum* fix = try_as<Fixnum>(duration)) {
      if(!fix->positive_p()) {
        Exception::raise_argument_error(state, "time interval must be positive");
      }
      ts.tv_sec = fix->to_native();
    } else if(Float* flt = try_as<Float>(duration)) {
      if(flt->val < 0.0) {
        Exception::raise_argument_error(state, "time interval must be positive");
      }
      uint64_t nano = (uint64_t)(flt->val * NANOSECONDS);
      ts.tv_sec  =  (time_t)(nano / NANOSECONDS);
      ts.tv_nsec =    (long)(nano % NANOSECONDS);
    } else if(duration == G(undefined)) {
      use_timed_wait = false;
    } else {
      return Primitives::failure();
    }

    time_t start = time(0);

    if(use_timed_wait) {
      struct timeval tv = {0,0};
      gettimeofday(&tv, 0);

      uint64_t nano = ts.tv_nsec + tv.tv_usec * 1000;
      ts.tv_sec  += tv.tv_sec + nano / NANOSECONDS;
      ts.tv_nsec  = nano % NANOSECONDS;

      if(!state->park_timed(state, &ts)) return NULL;
    } else {
      if(!state->park(state)) return NULL;
    }

    if(!state->check_async(state)) return NULL;

    return Fixnum::from(time(0) - start);
  }

  Object* System::vm_check_interrupts(STATE) {
    if(state->check_async(state)) {
      return cNil;
    } else {
      return NULL;
    }
  }

  static inline double tv_to_dbl(struct timeval* tv) {
    return (double)tv->tv_sec + ((double)tv->tv_usec / 1000000.0);
  }

  Array* System::vm_times(STATE) {
#ifdef RBX_WINDOWS
    // TODO: Windows
    return force_as<Array>(Primitives::failure());
#else
    struct rusage buf;

    Array* ary = Array::create(state, 4);

    getrusage(RUSAGE_SELF, &buf);
    ary->set(state, 0, Float::create(state, tv_to_dbl(&buf.ru_utime)));
    ary->set(state, 1, Float::create(state, tv_to_dbl(&buf.ru_stime)));

    getrusage(RUSAGE_CHILDREN, &buf);
    ary->set(state, 2, Float::create(state, tv_to_dbl(&buf.ru_utime)));
    ary->set(state, 3, Float::create(state, tv_to_dbl(&buf.ru_stime)));

    uint64_t sys = 0;
    uint64_t usr = 0;
    thread_cpu_usage(&usr, &sys);

    ary->set(state, 4, Float::create(state, (double)usr / 1000000.0));
    ary->set(state, 5, Float::create(state, (double)sys / 1000000.0));

    return ary;
#endif
  }

  Class* System::vm_open_class(STATE, Symbol* name, Object* sup,
                               ConstantScope* scope)
  {
    Module* under;

    if(scope->nil_p()) {
      under = G(object);
    } else {
      under = scope->module();
    }

    return vm_open_class_under(state, name, sup, under);
  }

  Class* System::vm_open_class_under(STATE, Symbol* name, Object* super,
                                     Module* under)
  {
    ConstantMissingReason reason = vNonExistent;

    Object* obj = under->get_const(state, name, G(sym_private), &reason);
    if(reason == vFound) {
      Class* cls = as<Class>(obj);
      if(super->nil_p()) return cls;

      if(cls->true_superclass(state) != super) {
        std::ostringstream message;
        message << "Superclass mismatch: given "
                << as<Module>(super)->debug_str(state)
                << " but previously set to "
                << cls->true_superclass(state)->debug_str(state);

        Exception* exc =
          Exception::make_type_error(state, Class::type, super,
                                     message.str().c_str());
        // exc->locations(state, System::vm_backtrace(state,
        //                Fixnum::from(0));
        state->raise_exception(exc);
        return NULL;
      }

      return cls;
    }

    if(super->nil_p()) super = G(object);

    return Class::create(state, as<Class>(super), under, name);
  }

  Module* System::vm_open_module(STATE, Symbol* name, ConstantScope* scope) {
    Module* under = G(object);

    if(!scope->nil_p()) {
      under = scope->module();
    }

    return vm_open_module_under(state, name, under);
  }

  Module* System::vm_open_module_under(STATE, Symbol* name, Module* under) {
    ConstantMissingReason reason = vNonExistent;

    Object* obj = under->get_const(state, name, G(sym_private), &reason);

    if(reason == vFound) return as<Module>(obj);

    Module* module = Module::create(state);

    module->set_name(state, name, under);
    under->set_const(state, name, module);

    return module;
  }

  static Tuple* find_method(STATE, Module* lookup_begin, Symbol* name, Symbol* min_visibility) {
    // Use cUndef for the self type so protected checks never pass
    // and work as expected.
    LookupData lookup(cUndef, lookup_begin, min_visibility);

    Dispatch dispatch(name);

    if(!dispatch.resolve(state, name, lookup)) {
      return nil<Tuple>();
    }

    return Tuple::from(state, 2, dispatch.method, dispatch.module);
  }

  Tuple* System::vm_find_method(STATE, Object* recv, Symbol* name) {
    return find_method(state, recv->lookup_begin(state), name, G(sym_private));
  }

  Tuple* System::vm_find_public_method(STATE, Object* recv, Symbol* name) {

    return find_method(state, recv->lookup_begin(state), name, G(sym_public));
  }

  Object* System::vm_add_method(STATE, Symbol* name,
                                Object* method,
                                ConstantScope* scope, Object* vis)
  {
    Module* mod = scope->for_method_definition();

    CompiledCode* cc = try_as<CompiledCode>(method);
    if(cc) {
      cc->scope(state, scope);
      cc->serial(state, Fixnum::from(0));
      mod->add_method(state, name, nil<String>(), cc, scope);
    } else {
      mod->add_method(state, name, as<String>(method), cNil, scope);
    }

    vm_reset_method_cache(state, mod, name);

    if(!cc) return method;

    if(Class* cls = try_as<Class>(mod)) {
      OnStack<5> o2(state, mod, cc, scope, vis, cls);

      if(!cc->internalize(state)) {
        Exception::raise_argument_error(state, "invalid bytecode method");
        return 0;
      }

      object_type type = (object_type)cls->instance_type()->to_native();
      TypeInfo* ti = state->memory()->type_info[type];
      if(ti) {
        cc->specialize(state, ti);
      }
    }

    bool add_ivars = false;

    if(Class* cls = try_as<Class>(mod)) {
      add_ivars = !kind_of<SingletonClass>(cls) &&
                  cls->type_info()->type == Object::type;
    } else {
      add_ivars = true;
    }

    if(add_ivars) {
      Array* ary = mod->seen_ivars();
      if(ary->nil_p()) {
        ary = Array::create(state, 5);
        mod->seen_ivars(state, ary);
      }

      Tuple* lits = cc->literals();
      for(native_int i = 0; i < lits->num_fields(); i++) {
        if(Symbol* sym = try_as<Symbol>(lits->at(state, i))) {
          if(CBOOL(sym->is_ivar_p(state))) {
            if(!ary->includes_p(state, sym)) ary->append(state, sym);
          }
        }
      }
    }

    return cc;
  }

  Object* System::vm_attach_method(STATE, Symbol* name,
                                   Object* method,
                                   ConstantScope* scope, Object* recv)
  {
    Module* mod = recv->singleton_class(state);

    if(CompiledCode* cc = try_as<CompiledCode>(method)) {
      cc->scope(state, scope);
      cc->serial(state, Fixnum::from(0));
      mod->add_method(state, name, nil<String>(), cc, scope);
    } else {
      mod->add_method(state, name, as<String>(method), cNil, scope);
    }

    vm_reset_method_cache(state, mod, name);

    return method;
  }

  Class* System::vm_object_class(STATE, Object* obj) {
    return obj->class_object(state);
  }

  Object* System::vm_object_singleton_class(STATE, Object* obj) {
    if(obj->reference_p()) return obj->singleton_class(state);
    if(obj->true_p()) return G(true_class);
    if(obj->false_p()) return G(false_class);
    if(obj->nil_p()) return G(nil_class);
    return Primitives::failure();
  }

  Object* System::vm_singleton_class_object(STATE, Module* mod) {
    if(SingletonClass* sc = try_as<SingletonClass>(mod)) {
      return sc->singleton();
    }

    return cNil;
  }

  Object* System::vm_object_respond_to(STATE, Object* obj, Symbol* name, Object* include_private) {
    return obj->respond_to(state, name, include_private);
  }

  Object* System::vm_object_equal(STATE, Object* a, Object* b) {
    return RBOOL(a == b);
  }

  Object* System::vm_object_kind_of(STATE, Object* obj, Module* mod) {
    return RBOOL(obj->kind_of_p(state, mod));
  }

  Object* System::vm_global_serial(STATE) {
    return Fixnum::from(state->shared().global_serial());
  }

  Object* System::vm_inc_global_serial(STATE) {
    if(state->shared().config.serial_debug) {
      std::cerr << std::endl
                << "global serial increased from "
                << state->shared().global_serial()
                << std::endl;

      state->vm()->call_frame()->print_backtrace(state, std::cerr, 6, true);
    }

    return Fixnum::from(state->shared().inc_global_serial(state));
  }

  Object* System::vm_deoptimize_all(STATE, Object* o_disable) {
    memory::ObjectWalker walker(state->memory());
    memory::GCData gc_data(state->vm());

    // Seed it with the root objects.
    walker.seed(gc_data);

    Object* obj = walker.next();

    int total = 0;

    bool disable = CBOOL(o_disable);

    // TODO: this should be inside tooling
    bool tooling_interpreter = state->shared().tool_broker()->tooling_interpreter_p();

    while(obj) {
      if(CompiledCode* code = try_as<CompiledCode>(obj)) {
        if(MachineCode* mcode = code->machine_code()) {
          mcode->deoptimize(state, code, 0, disable);
          if(tooling_interpreter) {
            mcode->run = MachineCode::tooling_interpreter;
          } else {
            mcode->run = MachineCode::interpreter;
          }
        }
        total++;
      }

      obj = walker.next();
    }

    return Integer::from(state, total);
  }

  Object* System::vm_raise_exception(STATE, Exception* exc) {
    state->raise_exception(exc);
    return NULL;
  }

  Fixnum* System::vm_memory_size(STATE, Object* obj) {
    if(obj->reference_p()) {
      size_t bytes = obj->size_in_bytes(state->vm());
      if(Bignum* b = try_as<Bignum>(obj)) {
        bytes += b->managed_memory_size(state);
      }
      Object* iv = obj->ivars();
      if(LookupTable* lt = try_as<LookupTable>(iv)) {
        bytes += iv->size_in_bytes(state->vm());
        bytes += lt->values()->size_in_bytes(state->vm());
        bytes += (lt->entries()->to_native() * sizeof(LookupTableBucket));
      } else if(iv->reference_p()) {
        bytes += iv->size_in_bytes(state->vm());
      }
      return Fixnum::from(bytes);
    }

    return Fixnum::from(0);
  }

  Object* System::vm_throw(STATE, Object* dest, Object* value) {
    state->vm()->thread_state()->raise_throw(dest, value);
    return NULL;
  }

  Object* System::vm_catch(STATE, Object* dest, Object* obj) {
    LookupData lookup(obj, obj->lookup_begin(state), G(sym_protected));
    Dispatch dispatch(G(sym_call));
    Arguments args(G(sym_call), 1, &dest);
    args.set_recv(obj);

    OnStack<1> os(state, dest);
    Object* ret = dispatch.send(state, lookup, args);

    if(!ret && state->vm()->thread_state()->raise_reason() == cCatchThrow) {
      if(state->vm()->thread_state()->throw_dest() == dest) {
        Object* val = state->vm()->thread_state()->raise_value();
        state->vm()->thread_state()->clear_return();
        return val;
      }
    }

    return ret;
  }

  Object* System::vm_set_class(STATE, Object* obj, Class* cls) {
    if(!obj->reference_p()) return Primitives::failure();
    if(obj->type_id() != cls->type_info()->type) {
      return Primitives::failure();
    }

    if(kind_of<PackedObject>(obj)) {
      if(obj->klass()->packed_size() != cls->packed_size()) {
        return Primitives::failure();
      }
    }

    obj->klass(state, cls);
    return obj;
  }

  Object* System::vm_method_missing_reason(STATE) {
    switch(state->vm()->method_missing_reason()) {
    case ePrivate:
      return G(sym_private);
    case eProtected:
      return G(sym_protected);
    case eSuper:
      return state->symbol("super");
    case eVCall:
      return state->symbol("vcall");
    case eNormal:
      return state->symbol("normal");
    default:
      return state->symbol("none");
    }
  }

  Object* System::vm_constant_missing_reason(STATE) {
    switch(state->vm()->constant_missing_reason()) {
    case vPrivate:
      return G(sym_private);
    case vNonExistent:
      return state->symbol("normal");
    default:
      return state->symbol("none");
    }
  }

  Object* System::vm_extended_modules(STATE, Object* obj) {
    if(SingletonClass* sc = try_as<SingletonClass>(obj->klass())) {
      Array* ary = Array::create(state, 3);

      Module* mod = sc->superclass();
      while(IncludedModule* im = try_as<IncludedModule>(mod)) {
        ary->append(state, im->module());

        mod = mod->superclass();
      }

      return ary;
    }

    return cNil;
  }

  Object* System::vm_const_defined(STATE, Symbol* sym) {
    ConstantMissingReason reason = vNonExistent;

    Object* res = Helpers::const_get(state, sym, &reason);

    if(reason != vFound) {
      return Primitives::failure();
    }

    return res;
  }

  Object* System::vm_const_defined_under(STATE, Module* under, Symbol* sym,
                                         Object* send_const_missing)
  {
    ConstantMissingReason reason = vNonExistent;

    Object* res = Helpers::const_get_under(state, under, sym, &reason);
    if(reason != vFound) {
      if(send_const_missing->true_p()) {
        res = Helpers::const_missing_under(state, under, sym);
      } else {
        res = Primitives::failure();
      }
    }

    return res;
  }

  Object* System::vm_check_callable(STATE, Object* obj, Symbol* sym, Object* self) {
    LookupData lookup(self, obj->lookup_begin(state), G(sym_public));
    Dispatch dispatch(sym);

    Object* responds = RBOOL(dispatch.resolve(state, sym, lookup));
    if(!CBOOL(responds)) {
      LookupData lookup(obj, obj->lookup_begin(state), G(sym_private));
      Symbol* name = G(sym_respond_to_missing);
      Dispatch dispatch(name);

      Object* buf[2];
      buf[0] = name;
      buf[1] = G(sym_public);
      Arguments args(name, obj, 2, buf);
      responds = RBOOL(CBOOL(dispatch.send(state, lookup, args)));
    }
    return responds;
  }

  Object* System::vm_check_super_callable(STATE) {
    CallFrame* call_frame = state->vm()->call_frame();

    Module* start = call_frame->module()->superclass();
    Symbol* sym = call_frame->original_name();

    LookupData lookup(call_frame->self(), start, G(sym_private));
    Dispatch dispatch(sym);

    return RBOOL(dispatch.resolve(state, sym, lookup));
  }

#define GETPW_R_SIZE 2048

  String* System::vm_get_user_home(STATE, String* name) {
#ifdef RBX_WINDOWS
    // TODO: Windows
    return force_as<String>(Primitives::failure());
#else
    struct passwd pw;
    struct passwd *pwd;

    long len = sysconf(_SC_GETPW_R_SIZE_MAX);
    if(len < 0) len = GETPW_R_SIZE;

retry:
    ByteArray* buf =
      state->memory()->new_bytes<ByteArray>(state, G(bytearray), len);

    int err = getpwnam_r(name->c_str_null_safe(state), &pw,
                         reinterpret_cast<char*>(buf->raw_bytes()), len, &pwd);
    if(err) {
      if(errno == ERANGE) {
        len *= 2;
        // Check for overflow
        if(len > 0) goto retry;
        Exception::raise_runtime_error(state, "getpwnam_r(3) buffer exceeds maximum size");
      }
      Exception::raise_errno_error(state, "retrieving user home directory",
          errno, "getpwnam_r(3)");
    }
    if(pwd) {
      return String::create(state, pwd->pw_dir);
    }
    return nil<String>();
#endif
  }

  Object* System::vm_set_finalizer(STATE, Object* obj, Object* fin) {
    if(!obj->reference_p()) return cFalse;
    state->memory()->set_ruby_finalizer(obj, fin);
    return cTrue;
  }

  Object* System::vm_object_lock(STATE, Object* obj) {
    if(!obj->reference_p()) return Primitives::failure();

    switch(obj->lock(state)) {
    case eLocked:
      return cTrue;
    case eLockTimeout:
    case eUnlocked:
    case eLockError:
      return Primitives::failure();
    case eLockInterrupted:
      {
        Exception* exc = state->vm()->interrupted_exception();
        assert(!exc->nil_p());
        state->vm()->clear_interrupted_exception();
        exc->locations(state, Location::from_call_stack(state));
        state->raise_exception(exc);
        return 0;
      }
    }

    return cNil;
  }

  Object* System::vm_object_uninterrupted_lock(STATE, Object* obj) {
    if(!obj->reference_p()) return Primitives::failure();

retry:
    switch(obj->lock(state, false)) {
    case eLocked:
      return cTrue;
    case eLockInterrupted:
      goto retry;
    case eLockTimeout:
    case eUnlocked:
    case eLockError:
      return Primitives::failure();
    }

    return cNil;
  }

  Object* System::vm_object_lock_timed(STATE, Object* obj, Integer* time) {
    if(!obj->reference_p()) return Primitives::failure();

    switch(obj->lock(state, time->to_native())) {
    case eLocked:
      return cTrue;
    case eLockTimeout:
      return cFalse;
    case eUnlocked:
    case eLockError:
      return Primitives::failure();
    case eLockInterrupted:
      {
        Exception* exc = state->vm()->interrupted_exception();
        assert(!exc->nil_p());
        state->vm()->clear_interrupted_exception();
        exc->locations(state, Location::from_call_stack(state));
        state->raise_exception(exc);
        return 0;
      }
      return 0;
    }

    return cNil;
  }

  Object* System::vm_object_trylock(STATE, Object* obj) {
    if(!obj->reference_p()) return Primitives::failure();
    return RBOOL(obj->try_lock(state) == eLocked);
  }

  Object* System::vm_object_locked_p(STATE, Object* obj) {
    if(!obj->reference_p()) return cFalse;
    return RBOOL(obj->locked_p(state));
  }

  Object* System::vm_object_unlock(STATE, Object* obj) {
    if(!obj->reference_p()) return Primitives::failure();

    if(obj->unlock(state) == eUnlocked) return cNil;
    if(cDebugThreading) {
      std::cerr << "[LOCK " << state->vm()->thread_id() << " unlock failed]" << std::endl;
    }
    return Primitives::failure();
  }

  Object* System::vm_memory_barrier(STATE) {
    atomic::memory_barrier();
    return cNil;
  }

  Object* System::vm_windows_p(STATE) {
#ifdef RBX_WINDOWS
    return cTrue;
#else
    return cFalse;
#endif
  }

  Object* System::vm_darwin_p(STATE) {
#ifdef RBX_DARWIN
    return cTrue;
#else
    return cFalse;
#endif
  }

  Object* System::vm_bsd_p(STATE) {
#ifdef RBX_BSD
    return cTrue;
#else
    return cFalse;
#endif
  }

  Object* System::vm_linux_p(STATE) {
#ifdef RBX_LINUX
    return cTrue;
#else
    return cFalse;
#endif
  }

  static const char sha1_hex[] = {
     '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
     'a', 'b', 'c', 'd', 'e', 'f'
  };

  String* System::sha1_hash(STATE, String* str) {
    XSHA1_CTX ctx;
    XSHA1_Init(&ctx);
    XSHA1_Update(&ctx, str->byte_address(), str->byte_size());

    uint8_t digest[20];
    XSHA1_Finish(&ctx, digest);

    char buf[40];

    for(int i = 0; i < 20; i++) {
      unsigned char byte = digest[i];
      buf[i + i]     = sha1_hex[byte >> 4];
      buf[i + i + 1] = sha1_hex[byte & 0x0f];
    }

    return String::create(state, buf, 40);
  }

  Tuple* System::vm_thread_state(STATE) {
    VMThreadState* ts = state->vm()->thread_state();
    Tuple* tuple = state->memory()->new_fields<Tuple>(state, G(tuple), 5);

    Symbol* reason = 0;
    switch(ts->raise_reason()) {
    case cNone:
      reason = state->symbol("none");
      break;
    case cException:
      reason = state->symbol("exception");
      break;
    case cReturn:
      reason = state->symbol("return");
      break;
    case cBreak:
      reason = state->symbol("break");
      break;
    case cExit:
      reason = state->symbol("exit");
      break;
    case cCatchThrow:
      reason = state->symbol("catch_throw");
      break;
    case cThreadKill:
      reason = state->symbol("thread_kill");
      break;
    default:
      reason = state->symbol("unknown");
    }

    tuple->put(state, 0, reason);
    tuple->put(state, 1, ts->raise_value());
    tuple->put(state, 2, ts->destination_scope());
    tuple->put(state, 3, ts->current_exception());
    tuple->put(state, 4, ts->throw_dest());

    return tuple;
  }

  Object* System::vm_run_script(STATE, CompiledCode* code) {
    Arguments args(state->symbol("__script__"), G(main), cNil, 0, 0);

    OnStack<1> os(state, code);

    code->internalize(state, 0, 0);

#ifdef RBX_PROFILER
    if(unlikely(state->vm()->tooling())) {
      tooling::ScriptEntry me(state, code);
      return code->machine_code()->execute_as_script(state, code);
    } else {
      return code->machine_code()->execute_as_script(state, code);
    }
#else
    return code->machine_code()->execute_as_script(state, code);
#endif
  }

#define HASH_TRIE_BASE_SHIFT  6

#if RBX_SIZEOF_LONG == 8
#define HASH_TRIE_BIT_WIDTH   6
#define HASH_TRIE_BIT_MASK    0x3f
#else
#define HASH_TRIE_BIT_WIDTH   5
#define HASH_TRIE_BIT_MASK    0x1f
#endif

  static inline size_t hash_trie_bit(Fixnum* hash, Fixnum* level) {
    native_int h = hash->to_native();
    native_int l = level->to_native();

    size_t width = HASH_TRIE_BIT_WIDTH;
    size_t mask = HASH_TRIE_BIT_MASK;
    size_t base = HASH_TRIE_BASE_SHIFT;
    size_t result = 1;

    return result << ((h >> (l * width + base)) & mask);
  }

  static inline int hash_trie_index(size_t m) {
#if RBX_SIZEOF_LONG == 8
    native_int sk5 = 0x5555555555555555;
    native_int sk3 = 0x3333333333333333;
    native_int skf0 = 0x0F0F0F0F0F0F0F0F;

    m -= (m >> 1) & sk5;
    m = (m & sk3) + ((m >> 2) & sk3);
    m = (m & skf0) + ((m >> 4) & skf0);
    m += m >> 8;
    m += m >> 16;
    m = (m + (m >> 32)) & 0xFF;
#else
    native_int sk5 = 0x55555555;
    native_int sk3 = 0x33333333;
    native_int skf0 = 0xF0F0F0F;

    m -= (m >> 1) & sk5;
    m = (m & sk3) + ((m >> 2) & sk3);
    m = (m & skf0) + ((m >> 4) & skf0);
    m += m >> 8;
    m = (m + (m >> 16)) & 0x3F;
#endif

    return m;
  }

  Fixnum* System::vm_hash_trie_item_index(STATE, Fixnum* hash,
                                           Fixnum* level, Integer* map)
  {
    size_t m = map->to_ulong();
    size_t b = hash_trie_bit(hash, level);

    if(m & b) {
      return Fixnum::from(hash_trie_index((b - 1) & m));
    } else {
      return nil<Fixnum>();
    }
  }

  Integer* System::vm_hash_trie_set_bitmap(STATE, Fixnum* hash,
                                           Fixnum* level, Integer* map)
  {
    size_t m = map->to_ulong();
    size_t b = hash_trie_bit(hash, level);

    return Integer::from(state, m | b);
  }

  Integer* System::vm_hash_trie_unset_bitmap(STATE, Fixnum* hash,
                                             Fixnum* level, Integer* map)
  {
    size_t m = map->to_ulong();
    size_t b = hash_trie_bit(hash, level);

    return Integer::from(state, m & ~b);
  }

  String* System::vm_get_module_name(STATE, Module* mod) {
    return mod->get_name(state);
  }

  Object* System::vm_set_module_name(STATE, Module* mod, Object* name, Object* under) {
    if(name->nil_p()) return cNil;

    if(under->nil_p()) under = G(object);
    mod->set_name(state, as<Symbol>(name), as<Module>(under));

    return cNil;
  }

  String* System::vm_set_process_title(STATE, String* title) {
    setproctitle("%s", title->c_str_null_safe(state));
    return title;
  }

  Object* System::vm_dtrace_fire(STATE, String* payload) {
#if HAVE_DTRACE
    if(RUBINIUS_RUBY_PROBE_ENABLED()) {
      char* bytes = reinterpret_cast<char*>(payload->byte_address());
      RUBINIUS_RUBY_PROBE(
          const_cast<RBX_DTRACE_CHAR_P>(bytes),
          payload->byte_size());
      return cTrue;
    }
    return cFalse;
#else
    return cNil;
#endif
  }
}
