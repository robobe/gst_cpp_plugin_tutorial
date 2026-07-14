import queue


def offer_latest(target_queue, item):
    try:
        target_queue.put_nowait(item)
        return True
    except queue.Full:
        pass

    try:
        target_queue.get_nowait()
    except queue.Empty:
        pass

    try:
        target_queue.put_nowait(item)
        return True
    except queue.Full:
        return False
