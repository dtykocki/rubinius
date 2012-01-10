/* This MUST all be at the top */
#include "builtin/fiber.hpp"
/* end must be at the top */


#include "builtin/object.hpp"
#include "builtin/array.hpp"
#include "vm/vm.hpp"
#include "builtin/class.hpp"
#include "builtin/integer.hpp"
#include "builtin/exception.hpp"

#include "objectmemory.hpp"
#include "gc/gc.hpp"

#include "call_frame.hpp"
#include "arguments.hpp"

#include "ontology.hpp"

#include "on_stack.hpp"

namespace rubinius {

  void Fiber::init(STATE) {
    GO(fiber).set(ontology::new_class(state, "Fiber", G(object), G(rubinius)));
    G(fiber)->set_object_type(state, FiberType);

#ifdef FIBER_ENABLED
    G(fiber)->set_const(state, "ENABLED", Qtrue);
#else
    G(fiber)->set_const(state, "ENABLED", Qfalse);
#endif
  }

  Fiber* Fiber::current(STATE) {
#ifdef FIBER_ENABLED
    Fiber* fib = state->vm()->current_fiber.get();

    // Lazily allocate a root fiber.
    if(fib->nil_p()) {
      fib = state->new_object<Fiber>(G(fiber));
      if(fib->zone() != YoungObjectZone) {
        state->memory()->remember_object(fib);
      }
      fib->prev_ = nil<Fiber>();
      fib->top_ = 0;
      fib->root_ = true;
      fib->status_ = Fiber::eRunning;

      fib->data_ = new FiberData(state->vm(), true);

      state->memory()->needs_finalization(fib, (FinalizerFunction)&Fiber::finalize);

      state->vm()->current_fiber.set(fib);
      state->vm()->root_fiber.set(fib);
    }

    return fib;
#else
    return nil<Fiber>();
#endif
  }

  void Fiber::start_on_stack() {
#ifdef FIBER_ENABLED
    VM* vm = VM::current();
    State state(vm);

    Fiber* fib = Fiber::current(&state);

    OnStack<1> os(&state, fib);

    Array* result = nil<Array>();
    Object* obj = fib->starter()->send(&state, NULL, state.globals().sym_call.get(), fib->value(), Qnil, false);
    // GC has run! Don't use stack vars!

    fib = Fiber::current(&state);
    fib->top_ = 0;
    fib->status_ = Fiber::eDead;
    fib->set_ivar(&state, state.symbol("@dead"), Qtrue);

    Fiber* dest = fib->prev();
    assert(!dest->nil_p());

    // Box this up so it's in a standard format at the point
    // of returning, so we can deal with it in the same way
    // as *args from #yield, #resume, and #transfer
    if(obj) {
      result = Array::create(&state, 1);
      result->set(&state, 0, obj);
    } else {
      if(state.vm()->thread_state()->raise_reason() == cException) {
        dest->exception(&state, state.vm()->thread_state()->current_exception());
      }
    }

    dest->run();
    dest->value(&state, result);
    state.vm()->set_current_fiber(dest);

    dest->data_->switch_and_orphan(&state, fib->data_);

    assert(0 && "fatal start_on_stack error");
#else
    rubinius::bug("Fibers not supported on this platform");
#endif
  }

  Fiber* Fiber::create(STATE, Object* self, Integer* i_stack_size, Object* callable) {
#ifdef FIBER_ENABLED
    int stack_size = i_stack_size->to_native();

    if(stack_size < 1024 * 1024) {
      stack_size = 1024 * 1024;
    }

    Fiber* fib = state->new_object<Fiber>(as<Class>(self));
    if(fib->zone() != YoungObjectZone) {
      state->memory()->remember_object(fib);
    }
    fib->starter(state, callable);
    fib->prev(state, nil<Fiber>());
    fib->top_ = 0;
    fib->root_ = false;
    fib->status_ = Fiber::eSleeping;

    fib->data_ = 0;

    state->memory()->needs_finalization(fib, (FinalizerFunction)&Fiber::finalize);

    return fib;
#else
    return reinterpret_cast<Fiber*>(Primitives::failure());
#endif
  }

  Object* Fiber::resume(STATE, Arguments& args, CallFrame* calling_environment) {
#ifdef FIBER_ENABLED
    if(!data_) {
      data_ = state->vm()->new_fiber_data();
    }

    if(status_ == Fiber::eDead || data_->dead_p()) {
      Exception::fiber_error(state, "dead fiber called");
    }

    if(!prev_->nil_p()) {
      Exception::fiber_error(state, "double resume");
    }

    if(data_->thread() && data_->thread() != state->vm()) {
      Exception::fiber_error(state, "cross thread fiber resuming is illegal");
    }

    Array* val = args.as_array(state);
    value(state, val);

    Fiber* cur = Fiber::current(state);
    prev(state, cur);

    cur->sleep(calling_environment);

    run();
    state->vm()->set_current_fiber(this);

    data_->switch_to(state, cur->data_);

    // Back here when someone yields back to us!
    // Beware here, because the GC has probably run so GC pointers on the C++ stack
    // can't be accessed.

    cur = Fiber::current(state);

    if(!cur->exception()->nil_p()) {
      state->raise_exception(cur->exception());
      cur->exception(state, nil<Exception>());
      return 0;
    }

    Array* ret = cur->value();

    if(ret->nil_p()) return Qnil;

    switch(ret->size()) {
    case 0:  return Qnil;
    case 1:  return ret->get(state, 0);
    default: return ret;
    }
#else
    return Primitives::failure();
#endif
  }

  Object* Fiber::transfer(STATE, Arguments& args, CallFrame* calling_environment) {
#ifdef FIBER_ENABLED
    if(!data_) {
      data_ = state->vm()->new_fiber_data();
    }

    if(status_ == Fiber::eDead || data_->dead_p()) {
      Exception::fiber_error(state, "dead fiber called");
    }

    if(data_->thread() && data_->thread() != state->vm()) {
      Exception::fiber_error(state, "cross thread fiber resuming is illegal");
    }

    Array* val = args.as_array(state);
    value(state, val);

    Fiber* cur = Fiber::current(state);
    Fiber* root = state->vm()->root_fiber.get();
    assert(root);

    prev(state, root);

    cur->sleep(calling_environment);

    run();
    state->vm()->set_current_fiber(this);

    data_->switch_to(state, cur->data_);

    // Back here when someone transfers back to us!
    // Beware here, because the GC has probably run so GC pointers on the C++ stack
    // can't be accessed.

    cur = Fiber::current(state);

    if(!cur->exception()->nil_p()) {
      state->raise_exception(cur->exception());
      cur->exception(state, nil<Exception>());
      return 0;
    }

    Array* ret = cur->value();

    if(ret->nil_p()) return Qnil;

    switch(ret->size()) {
    case 0:  return Qnil;
    case 1:  return ret->get(state, 0);
    default: return ret;
    }
#else
    return Primitives::failure();
#endif
  }

  Object* Fiber::s_yield(STATE, Arguments& args, CallFrame* calling_environment) {
#ifdef FIBER_ENABLED
    Fiber* cur = Fiber::current(state);
    Fiber* dest_fib = cur->prev();

    assert(cur != dest_fib);

    if(cur->root_) {
      Exception::fiber_error(state, "can't yield from root fiber");
    }

    cur->prev(state, nil<Fiber>());

    Array* val = args.as_array(state);
    dest_fib->value(state, val);

    cur->sleep(calling_environment);

    dest_fib->run();
    state->vm()->set_current_fiber(dest_fib);

    dest_fib->data_->switch_to(state, cur->data_);

    // Back here when someone yields back to us!
    // Beware here, because the GC has probably run so GC pointers on the C++ stack
    // can't be accessed.

    cur = Fiber::current(state);

    Array* ret = cur->value();

    if(ret->nil_p()) return Qnil;

    switch(ret->size()) {
    case 0:  return Qnil;
    case 1:  return ret->get(state, 0);
    default: return ret;
    }
#else
    return Primitives::failure();
#endif
  }


  void Fiber::finalize(STATE, Fiber* fib) {
#ifdef FIBER_ENABLED
    if(!fib->data_) return;
    fib->data_->orphan(state);
    delete fib->data_;
#endif
  }

  void Fiber::Info::mark(Object* obj, ObjectMark& mark) {
    auto_mark(obj, mark);

    mark.remember_object(obj);

    Fiber* fib = (Fiber*)obj;

    if(!fib->data_) return;

    AddressDisplacement dis(fib->data_->data_offset(),
                            fib->data_->data_lower_bound(),
                            fib->data_->data_upper_bound());

    if(CallFrame* cf = fib->call_frame()) {
      mark.gc->walk_call_frame(cf, &dis);
    }

    mark.gc->scan(fib->data_->variable_root_buffers(), false, &dis);
  }
}

