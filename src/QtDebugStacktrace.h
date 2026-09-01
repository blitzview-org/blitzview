#pragma once
#include <QString>
#include <QDebug>
#include <QtGlobal>
#include <QMessageLogContext>
#include <QCoreApplication>
#include <cstdint>

// Symbolised stack traces for one specific Qt warning. Platform support is
// optional: where neither backend is available the handler still forwards the
// message, it just cannot say where it came from.
#if defined(Q_OS_WIN)
#  define BLITZVIEW_STACKTRACE_WIN 1
#  include <windows.h>
#  include <dbghelp.h>
#elif defined(__has_include)
#  if __has_include(<execinfo.h>) && __has_include(<dlfcn.h>)
#    define BLITZVIEW_STACKTRACE_POSIX 1
#    include <execinfo.h>
#    include <dlfcn.h>
#  endif
#endif

inline void qtDebugStacktraceHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (msg.contains("Empty filename passed to function")) {
        qWarning().noquote() << "--- Stacktrace for 'Empty filename passed to function' ---";
#if defined(BLITZVIEW_STACKTRACE_POSIX)
        void* trace[32];
        int n = backtrace(trace, 32);
        for (int i = 0; i < n; ++i) {
            Dl_info info;
            if (dladdr(trace[i], &info) && info.dli_sname) {
                QString func = info.dli_sname;
                QString file = info.dli_fname ? info.dli_fname : QString();
                qWarning().noquote() << QString("%1 (%2)").arg(func, file);
            } else {
                qWarning().noquote() << QString("?? (%1)").arg(QString::number((uintptr_t)trace[i], 16));
            }
        }
#elif defined(BLITZVIEW_STACKTRACE_WIN)
        // Same output shape as the POSIX branch so traces stay comparable
        // across platforms. Symbol names need debug info next to the exe; a
        // stripped release build degrades to the "?? (address)" form.
        void* trace[32];
        const USHORT n = CaptureStackBackTrace(0, 32, trace, nullptr);

        const HANDLE process = GetCurrentProcess();
        static bool symInitialized = false;
        if (!symInitialized) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(process, nullptr, TRUE);
            symInitialized = true;
        }

        // SYMBOL_INFO carries the name inline past the end of the struct.
        alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen   = MAX_SYM_NAME;

        for (USHORT i = 0; i < n; ++i) {
            const auto address = reinterpret_cast<DWORD64>(trace[i]);
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol)) {
                QString func = QString::fromLocal8Bit(symbol->Name);
                QString file;
                IMAGEHLP_MODULE64 module;
                module.SizeOfStruct = sizeof(module);
                if (SymGetModuleInfo64(process, address, &module))
                    file = QString::fromLocal8Bit(module.ImageName);
                qWarning().noquote() << QString("%1 (%2)").arg(func, file);
            } else {
                qWarning().noquote() << QString("?? (%1)").arg(QString::number(address, 16));
            }
        }
#else
        qWarning().noquote() << "(no stacktrace backend on this platform)";
#endif
        qWarning() << "--- End Stacktrace ---";
    }
    qt_message_output(type, ctx, msg);
    return;
}
