#pragma once
// Minimal RAII wrappers for native Win32 handles.

#include <windows.h>
#include <utility>

// Wraps a HANDLE that is invalid when equal to INVALID_HANDLE_VALUE and closed with CloseHandle.
class UniqueHandle {
public:
    UniqueHandle() noexcept : m_handle(INVALID_HANDLE_VALUE) {}
    explicit UniqueHandle(HANDLE h) noexcept : m_handle(h) {}

    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    bool IsValid() const noexcept { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }
    HANDLE Get() const noexcept { return m_handle; }

    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (IsValid()) {
            CloseHandle(m_handle);
        }
        m_handle = h;
    }

    HANDLE Release() noexcept {
        HANDLE h = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return h;
    }

private:
    HANDLE m_handle;
};

// Wraps a search handle returned by FindFirstFileExW, closed with FindClose.
class UniqueFindHandle {
public:
    UniqueFindHandle() noexcept : m_handle(INVALID_HANDLE_VALUE) {}
    explicit UniqueFindHandle(HANDLE h) noexcept : m_handle(h) {}

    ~UniqueFindHandle() { Reset(); }

    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

    UniqueFindHandle(UniqueFindHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    UniqueFindHandle& operator=(UniqueFindHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    bool IsValid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }
    HANDLE Get() const noexcept { return m_handle; }

    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (IsValid()) {
            FindClose(m_handle);
        }
        m_handle = h;
    }

private:
    HANDLE m_handle;
};

// Wraps a GDI object (HFONT, HBRUSH, etc.) deleted with DeleteObject.
template <typename T>
class UniqueGdiObject {
public:
    UniqueGdiObject() noexcept : m_obj(nullptr) {}
    explicit UniqueGdiObject(T obj) noexcept : m_obj(obj) {}

    ~UniqueGdiObject() { Reset(); }

    UniqueGdiObject(const UniqueGdiObject&) = delete;
    UniqueGdiObject& operator=(const UniqueGdiObject&) = delete;

    UniqueGdiObject(UniqueGdiObject&& other) noexcept : m_obj(other.m_obj) {
        other.m_obj = nullptr;
    }

    UniqueGdiObject& operator=(UniqueGdiObject&& other) noexcept {
        if (this != &other) {
            Reset();
            m_obj = other.m_obj;
            other.m_obj = nullptr;
        }
        return *this;
    }

    T Get() const noexcept { return m_obj; }
    bool IsValid() const noexcept { return m_obj != nullptr; }

    void Reset(T obj = nullptr) noexcept {
        if (m_obj != nullptr) {
            DeleteObject(m_obj);
        }
        m_obj = obj;
    }

private:
    T m_obj;
};
