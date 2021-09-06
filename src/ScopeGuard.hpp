#ifndef SCOPE_GUARD_HPP
#define SCOPE_GUARD_HPP

#include <functional>
#include <qglobal.h>


namespace detail
{


class ScopeGuardImpl
{
public:
	template<typename Func>
	explicit ScopeGuardImpl(Func &&func)
		: m_func(std::forward<Func>(func))
		, m_dismissed(false)
	{
	}

	~ScopeGuardImpl()
	{
		if (!m_dismissed)
		{
#ifdef QT_NO_EXCEPTIONS
			m_func();
#else
			// Make sure exceptions never exit the destructor, otherwise
			// undefined behavior occurs. For details about this, see
			// http://bin-login.name/ftp/pub/docs/programming_languages/cpp/cffective_cpp/MEC/MI11_FR.HTM
			try
			{
				m_func();
			}
			catch (...)
			{
			}
#endif
		}
	}

	ScopeGuardImpl(ScopeGuardImpl &&other)
		: m_func(std::move(other.m_func))
		, m_dismissed(other.m_dismissed)
	{
		other.m_dismissed = true;
	}

	/// Dismisses the scope guard, which will do nothing after this was called.
	void dismiss() const throw()
	{
		m_dismissed = true;
	}


private:
	ScopeGuardImpl(ScopeGuardImpl const &) = delete;
	ScopeGuardImpl& operator = (ScopeGuardImpl const &) = delete;

	std::function<void()> m_func;
	mutable bool m_dismissed;
};


} // namespace detail end


template<typename Func>
detail::ScopeGuardImpl makeScopeGuard(Func &&func)
{
	return detail::ScopeGuardImpl(std::forward<Func>(func));
}


#endif // SCOPE_GUARD_HPP
