class CredishError(Exception):
    pass

class ResponseError(CredishError):
    pass

class DataError(CredishError):
    pass

class NoKeyError(CredishError):
    pass

class WrongTypeError(ResponseError):
    """Operation against a key holding the wrong kind of value."""
    pass
