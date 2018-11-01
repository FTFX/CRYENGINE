// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

namespace CryAudio
{
namespace Impl
{
/**
 * An implementation may use this interface to define a class for storing implementation-specific
 * data needed for identifying and using the corresponding IEnvironment
 * (e.g. a middleware-specific bus ID or name to be passed to an API function)
 */
struct IEnvironment
{
	/** @cond */
	virtual ~IEnvironment() = default;
	/** @endcond */
};
} // namespace Impl
} // namespace CryAudio