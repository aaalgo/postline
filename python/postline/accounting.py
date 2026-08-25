def _response_to_dict(response):
    if hasattr(response, "to_dict"):
        return response.to_dict()
    if hasattr(response, "model_dump"):
        return response.model_dump()
    return response


def _get_usage_value(usage, key, default=None):
    if isinstance(usage, dict):
        return usage.get(key, default)
    return getattr(usage, key, default)


def estimate_memory_usage(response, context_window=None):
    """
    Estimate LLM context-memory usage as a percentage of the model context
    window, using prompt/input tokens from the provider response.

    Returns None when the response does not expose enough data.  This is meant
    to be a lightweight runtime signal for Postline messages, not an exact
    replayable state value.
    """

    response = _response_to_dict(response)
    usage = response.get("usage", {}) if isinstance(response, dict) else {}

    used_tokens = (
        _get_usage_value(usage, "prompt_tokens")
        or _get_usage_value(usage, "input_tokens")
    )
    if used_tokens is None:
        return None

    if context_window is None:
        context_window = _get_usage_value(usage, "context_window")
        if context_window is None and isinstance(response, dict):
            context_window = (
                response.get("context_window")
                or response.get("context_length")
            )
    if not context_window:
        return None

    try:
        return 100.0 * float(used_tokens) / float(context_window)
    except (TypeError, ValueError, ZeroDivisionError):
        return None


def updateMemoryUsage(response, message, context_window=None):
    usage = estimate_memory_usage(response, context_window)
    if usage is None:
        return
    message.updateHeader({
        "Postline-Memory-Usage": f"{usage:.1f}%",
    })


def updateAccounting(response, message, provider, context_window=None):
    """
    Extract accounting-related fields from a completion response and store them
    into Postline headers.
    """

    response = _response_to_dict(response)

    model = response["model"]
    usage = response.get("usage", {})

    accounting = {}

    for key, value in usage.items():
        if isinstance(value, dict):
            for subkey, subvalue in value.items():
                accounting[f"{key}.{subkey}"] = subvalue
        else:
            accounting[key] = value

    prefix = f"Postline-Cost:{provider}:{model}:"

    headers = {}
    headers['API-Provider'] = provider
    headers['Model-Used'] = model

    for key, value in accounting.items():
        headers[prefix + key] = str(value)

    message.updateHeader(headers)
    updateMemoryUsage(response, message, context_window)
