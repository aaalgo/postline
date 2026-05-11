# postline/__init__.py

from ._postline import *

def updateAccounting(response, message, provider):
    """
    Extract accounting-related fields from a chat completion response
    and store them into Postline headers.
    """

    if hasattr(response, "to_dict"):
        response = response.to_dict()

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


