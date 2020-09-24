#include "common.h"
#ifdef MODSECTION
#undef MODSECTION
#define MODSECTION "-ctrl"
#endif

#include "ctrl_pipe.h"
#include "version.h"
#include "blk-snap-ctl.h"
#include "snapstore.h"
#include "big_buffer.h"

#include <linux/poll.h>
#include <linux/uuid.h>

#define CMD_TO_USER_FIFO_SIZE 1024

ssize_t ctrl_pipe_command_initiate(ctrl_pipe_t *pipe, const char __user *buffer, size_t length);
ssize_t ctrl_pipe_command_next_portion(ctrl_pipe_t *pipe, const char __user *buffer, size_t length);
#ifdef CONFIG_BLK_SNAP_SNAPSTORE_MULTIDEV
ssize_t ctrl_pipe_command_next_portion_multidev(ctrl_pipe_t *pipe, const char __user *buffer,
						size_t length);
#endif

void ctrl_pipe_request_acknowledge(ctrl_pipe_t *pipe, unsigned int result);
void ctrl_pipe_request_invalid(ctrl_pipe_t *pipe);

LIST_HEAD(ctl_pipes);
DECLARE_RWSEM(ctl_pipes_lock);

void ctrl_pipe_done(void)
{
	bool is_empty;

	pr_err("Ctrl pipes - done\n");

	down_write(&ctl_pipes_lock);
	is_empty = list_empty(&ctl_pipes);
	up_write(&ctl_pipes_lock);

	//BUG_ON(!is_empty)
	if (!is_empty)
		pr_err("Unable to perform ctrl pipes cleanup: container is not empty\n");
}

static void ctrl_pipe_release_cb(struct kref *kref)
{
	ctrl_pipe_t *pipe = container_of(kref, ctrl_pipe_t, refcount);

	down_write(&ctl_pipes_lock);
	list_del(&pipe->link);
	up_write(&ctl_pipes_lock);

	kfifo_free(&pipe->cmd_to_user);

	kfree(pipe);
}

ctrl_pipe_t *ctrl_pipe_get_resource(ctrl_pipe_t *pipe)
{
	if (pipe)
		kref_get(&pipe->refcount);

	return pipe;
}

void ctrl_pipe_put_resource(ctrl_pipe_t *pipe)
{
	if (pipe)
		kref_put(&pipe->refcount, ctrl_pipe_release_cb);
}

ctrl_pipe_t *ctrl_pipe_new(void)
{
	int ret;
	ctrl_pipe_t *pipe = kmalloc(sizeof(ctrl_pipe_t), GFP_KERNEL);

	if (NULL == pipe) {
		pr_err("Failed to create new ctrl pipe: not enough memory\n");
		return NULL;
	}
	INIT_LIST_HEAD(&pipe->link);

	ret = kfifo_alloc(&pipe->cmd_to_user, CMD_TO_USER_FIFO_SIZE, GFP_KERNEL);
	if (ret) {
		pr_err("Failed to allocate fifo. errno=%d.\n", ret);
		kfree(pipe);
		return NULL;
	}
	spin_lock_init(&pipe->cmd_to_user_lock);

	kref_init(&pipe->refcount);

	init_waitqueue_head(&pipe->readq);

	down_write(&ctl_pipes_lock);
	list_add_tail(&pipe->link, &ctl_pipes);
	up_write(&ctl_pipes_lock);

	return pipe;
}

ssize_t ctrl_pipe_read(ctrl_pipe_t *pipe, char __user *buffer, size_t length)
{
	int ret;
	unsigned int processed = 0;

	if (kfifo_is_empty_spinlocked(&pipe->cmd_to_user, &pipe->cmd_to_user_lock)) {
		//nothing to read
		if (wait_event_interruptible(pipe->readq,
					     !kfifo_is_empty_spinlocked(&pipe->cmd_to_user,
									&pipe->cmd_to_user_lock))) {
			pr_err("Unable to wait for pipe read queue: interrupt signal was received\n");
			return -ERESTARTSYS;
		}
	}

	ret = kfifo_to_user(&pipe->cmd_to_user, buffer, length, &processed);
	if (ret) {
		pr_err("Failed to read command from ctrl pipe\n");
		return ret;
	}

	return (ssize_t)processed;
}

ssize_t ctrl_pipe_write(ctrl_pipe_t *pipe, const char __user *buffer, size_t length)
{
	ssize_t processed = 0;

	do {
		unsigned int command;

		if ((length - processed) < 4) {
			pr_err("Unable to write command to ctrl pipe: invalid command length=%lu\n",
			       length);
			break;
		}
		if (0 != copy_from_user(&command, buffer + processed, sizeof(unsigned int))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += sizeof(unsigned int);
		//+4
		switch (command) {
		case VEEAMSNAP_CHARCMD_INITIATE: {
			ssize_t res = ctrl_pipe_command_initiate(pipe, buffer + processed,
								 length - processed);
			if (res >= 0)
				processed += res;
			else
				processed = res;
		} break;
		case VEEAMSNAP_CHARCMD_NEXT_PORTION: {
			ssize_t res = ctrl_pipe_command_next_portion(pipe, buffer + processed,
								     length - processed);
			if (res >= 0)
				processed += res;
			else
				processed = res;
		} break;
#ifdef CONFIG_BLK_SNAP_SNAPSTORE_MULTIDEV
		case VEEAMSNAP_CHARCMD_NEXT_PORTION_MULTIDEV: {
			ssize_t res = ctrl_pipe_command_next_portion_multidev(
				pipe, buffer + processed, length - processed);
			if (res >= 0)
				processed += res;
			else
				processed = res;
		} break;
#endif
		default:
			pr_err("Ctrl pipe write error: invalid command [0x%x] received\n", command);
			break;
		}
	} while (false);
	return processed;
}

unsigned int ctrl_pipe_poll(ctrl_pipe_t *pipe)
{
	unsigned int mask = 0;

	if (!kfifo_is_empty_spinlocked(&pipe->cmd_to_user, &pipe->cmd_to_user_lock)) {
		mask |= (POLLIN | POLLRDNORM); /* readable */
	}
	mask |= (POLLOUT | POLLWRNORM); /* writable */

	return mask;
}

ssize_t ctrl_pipe_command_initiate(ctrl_pipe_t *pipe, const char __user *buffer, size_t length)
{
	int result = SUCCESS;
	ssize_t processed = 0;

	char *kernel_buffer = kmalloc(length, GFP_KERNEL);
	if (kernel_buffer == NULL) {
		pr_err("Unable to send next portion to pipe: cannot allocate buffer. length=%lu\n",
		       length);
		return -ENOMEM;
	}

	if (0 != copy_from_user(kernel_buffer, buffer, length)) {
		kfree(kernel_buffer);
		pr_err("Unable to write to pipe: invalid user buffer\n");
		return -EINVAL;
	}

	do {
		u64 stretch_empty_limit;
		unsigned int dev_id_list_length;
		uuid_t *unique_id;
		struct ioctl_dev_id_s *snapstore_dev_id;
		struct ioctl_dev_id_s *dev_id_list;

		//get snapstore uuid
		if ((length - processed) < 16) {
			pr_err("Unable to get snapstore uuid: invalid ctrl pipe initiate command. length=%lu\n",
			       length);
			break;
		}
		unique_id = (uuid_t *)(kernel_buffer + processed);
		processed += 16;

		//get snapstore empty limit
		if ((length - processed) < sizeof(u64)) {
			pr_err("Unable to get stretch snapstore limit: invalid ctrl pipe initiate command. length=%lu\n",
			       length);
			break;
		}
		stretch_empty_limit = *(u64 *)(kernel_buffer + processed);
		processed += sizeof(u64);

		//get snapstore device id
		if ((length - processed) < sizeof(struct ioctl_dev_id_s)) {
			pr_err("Unable to get snapstore device id: invalid ctrl pipe initiate command. length=%lu\n",
			       length);
			break;
		}
		snapstore_dev_id = (struct ioctl_dev_id_s *)(kernel_buffer + processed);
		processed += sizeof(struct ioctl_dev_id_s);

		//get device id list length
		if ((length - processed) < 4) {
			pr_err("Unable to get device id list length: ivalid ctrl pipe initiate command. length=%lu\n",
			       length);
			break;
		}
		dev_id_list_length = *(unsigned int *)(kernel_buffer + processed);
		processed += sizeof(unsigned int);

		//get devices id list
		if ((length - processed) < (dev_id_list_length * sizeof(struct ioctl_dev_id_s))) {
			pr_err("Unable to get all devices from device id list: invalid ctrl pipe initiate command. length=%lu\n",
			       length);
			break;
		}
		dev_id_list = (struct ioctl_dev_id_s *)(kernel_buffer + processed);
		processed += (dev_id_list_length * sizeof(struct ioctl_dev_id_s));

		{
			size_t inx;
			dev_t *dev_set;
			size_t dev_id_set_length = (size_t)dev_id_list_length;
			dev_t snapstore_dev;
			size_t dev_id_set_buffer_size;

			if ((snapstore_dev_id->major == -1) && (snapstore_dev_id->minor == -1))
				snapstore_dev = 0xFFFFffff; //multidevice
			else if ((snapstore_dev_id->major == 0) && (snapstore_dev_id->minor == 0))
				snapstore_dev = 0; //in memory
			else
				snapstore_dev =
					MKDEV(snapstore_dev_id->major, snapstore_dev_id->minor);

			dev_id_set_buffer_size = sizeof(dev_t) * dev_id_set_length;
			dev_set = kzalloc(dev_id_set_buffer_size, GFP_KERNEL);
			if (NULL == dev_set) {
				pr_err("Unable to process stretch snapstore initiation command: cannot allocate memory\n");
				result = -ENOMEM;
				break;
			}

			for (inx = 0; inx < dev_id_set_length; ++inx)
				dev_set[inx] =
					MKDEV(dev_id_list[inx].major, dev_id_list[inx].minor);

			result = snapstore_create(unique_id, snapstore_dev, dev_set,
						  dev_id_set_length);
			kfree(dev_set);
			if (result != SUCCESS) {
				pr_err("Failed to create snapstore on device [%d:%d]\n",
				       MAJOR(snapstore_dev), MINOR(snapstore_dev));
				break;
			}

			result = snapstore_stretch_initiate(
				unique_id, pipe, (sector_t)to_sectors(stretch_empty_limit));
			if (result != SUCCESS) {
				pr_err("Failed to initiate stretch snapstore %pUB\n", unique_id);
				break;
			}
		}
	} while (false);
	kfree(kernel_buffer);
	ctrl_pipe_request_acknowledge(pipe, result);

	if (result == SUCCESS)
		return processed;
	return result;
}

ssize_t ctrl_pipe_command_next_portion(ctrl_pipe_t *pipe, const char __user *buffer, size_t length)
{
	int result = SUCCESS;
	ssize_t processed = 0;
	struct big_buffer *ranges = NULL;

	do {
		uuid_t unique_id;
		unsigned int ranges_length;
		size_t ranges_buffer_size;

		//get snapstore id
		if ((length - processed) < 16) {
			pr_err("Unable to get snapstore id: invalid ctrl pipe next portion command. length=%lu\n",
			       length);
			break;
		}
		if (0 != copy_from_user(&unique_id, buffer + processed, sizeof(uuid_t))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += 16;

		//get ranges length
		if ((length - processed) < 4) {
			pr_err("Unable to get device id list length: invalid ctrl pipe next portion command. length=%lu\n",
			       length);
			break;
		}
		if (0 != copy_from_user(&ranges_length, buffer + processed, sizeof(unsigned int))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += sizeof(unsigned int);

		ranges_buffer_size = ranges_length * sizeof(struct ioctl_range_s);

		// ranges
		if ((length - processed) < (ranges_buffer_size)) {
			pr_err("Unable to get all ranges: invalid ctrl pipe next portion command. length=%lu\n",
			       length);
			break;
		}
		ranges = big_buffer_alloc(ranges_buffer_size, GFP_KERNEL);
		if (ranges == NULL) {
			pr_err("Unable to allocate page array buffer: failed to process next portion command\n");
			processed = -ENOMEM;
			break;
		}
		if (ranges_buffer_size !=
		    big_buffer_copy_from_user(buffer + processed, 0, ranges, ranges_buffer_size)) {
			pr_err("Unable to process next portion command: invalid user buffer for parameters\n");
			processed = -EINVAL;
			break;
		}
		processed += ranges_buffer_size;

		{
			result = snapstore_add_file(&unique_id, ranges, ranges_length);

			if (result != SUCCESS) {
				pr_err("Failed to add file to snapstore\n");
				result = -ENODEV;
				break;
			}
		}
	} while (false);
	if (ranges)
		big_buffer_free(ranges);

	if (result == SUCCESS)
		return processed;
	return result;
}
#ifdef CONFIG_BLK_SNAP_SNAPSTORE_MULTIDEV
ssize_t ctrl_pipe_command_next_portion_multidev(ctrl_pipe_t *pipe, const char __user *buffer,
						size_t length)
{
	int result = SUCCESS;
	ssize_t processed = 0;
	struct big_buffer *ranges = NULL;

	do {
		uuid_t unique_id;
		int snapstore_major;
		int snapstore_minor;
		unsigned int ranges_length;
		size_t ranges_buffer_size;

		//get snapstore id
		if ((length - processed) < 16) {
			pr_err("Unable to get snapstore id: invalid ctrl pipe next portion command. length=%lu\n",
			       length);
			break;
		}
		if (0 != copy_from_user(&unique_id, buffer + processed, sizeof(uuid_t))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += 16;

		//get device id
		if ((length - processed) < 8) {
			pr_err("Unable to get device id list length: invalid ctrl pipe next portion command. length=%lu\n",
			       length);
			break;
		}
		if (0 !=
		    copy_from_user(&snapstore_major, buffer + processed, sizeof(unsigned int))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += sizeof(unsigned int);

		if (0 !=
		    copy_from_user(&snapstore_minor, buffer + processed, sizeof(unsigned int))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += sizeof(unsigned int);

		//get ranges length
		if ((length - processed) < 4) {
			pr_err("Unable to get device id list length: invalid ctrl pipe next portion command. length=%lu\n",
			       length);
			break;
		}
		if (0 != copy_from_user(&ranges_length, buffer + processed, sizeof(unsigned int))) {
			pr_err("Unable to write to pipe: invalid user buffer\n");
			processed = -EINVAL;
			break;
		}
		processed += sizeof(unsigned int);

		ranges_buffer_size = ranges_length * sizeof(struct ioctl_range_s);

		// ranges
		if ((length - processed) < (ranges_buffer_size)) {
			pr_err("Unable to get all ranges: invalid ctrl pipe next portion command.  length=%lu\n",
			       length);
			break;
		}
		ranges = big_buffer_alloc(ranges_buffer_size, GFP_KERNEL);
		if (ranges == NULL) {
			pr_err("Unable to process next portion command: failed to allocate page array buffer\n");
			processed = -ENOMEM;
			break;
		}
		if (ranges_buffer_size !=
		    big_buffer_copy_from_user(buffer + processed, 0, ranges, ranges_buffer_size)) {
			pr_err("Unable to process next portion command: invalid user buffer from parameters\n");
			processed = -EINVAL;
			break;
		}
		processed += ranges_buffer_size;

		{
			result = snapstore_add_multidev(&unique_id,
							MKDEV(snapstore_major, snapstore_minor),
							ranges, ranges_length);

			if (result != SUCCESS) {
				pr_err("Failed to add file to snapstore\n");
				result = -ENODEV;
				break;
			}
		}
	} while (false);
	if (ranges)
		big_buffer_free(ranges);

	if (result == SUCCESS)
		//log_traceln_sz( "processed=", processed );
		return processed;
	return result;
}
#endif

void ctrl_pipe_push_request(ctrl_pipe_t *pipe, unsigned int *cmd, size_t cmd_len)
{
	kfifo_in_spinlocked(&pipe->cmd_to_user, cmd, (cmd_len * sizeof(unsigned int)),
			    &pipe->cmd_to_user_lock);

	wake_up(&pipe->readq);
}

void ctrl_pipe_request_acknowledge(ctrl_pipe_t *pipe, unsigned int result)
{
	unsigned int cmd[2];

	cmd[0] = VEEAMSNAP_CHARCMD_ACKNOWLEDGE;
	cmd[1] = result;

	ctrl_pipe_push_request(pipe, cmd, 2);
}

void ctrl_pipe_request_halffill(ctrl_pipe_t *pipe, unsigned long long filled_status)
{
	unsigned int cmd[3];

	pr_err("Snapstore is half-full\n");

	cmd[0] = (unsigned int)VEEAMSNAP_CHARCMD_HALFFILL;
	cmd[1] = (unsigned int)(filled_status & 0xFFFFffff); //lo
	cmd[2] = (unsigned int)(filled_status >> 32);

	ctrl_pipe_push_request(pipe, cmd, 3);
}

void ctrl_pipe_request_overflow(ctrl_pipe_t *pipe, unsigned int error_code,
				unsigned long long filled_status)
{
	unsigned int cmd[4];

	pr_err("Snapstore overflow\n");

	cmd[0] = (unsigned int)VEEAMSNAP_CHARCMD_OVERFLOW;
	cmd[1] = error_code;
	cmd[2] = (unsigned int)(filled_status & 0xFFFFffff); //lo
	cmd[3] = (unsigned int)(filled_status >> 32);

	ctrl_pipe_push_request(pipe, cmd, 4);
}

void ctrl_pipe_request_terminate(ctrl_pipe_t *pipe, unsigned long long filled_status)
{
	unsigned int cmd[3];

	pr_err("Snapstore termination\n");

	cmd[0] = (unsigned int)VEEAMSNAP_CHARCMD_TERMINATE;
	cmd[1] = (unsigned int)(filled_status & 0xFFFFffff); //lo
	cmd[2] = (unsigned int)(filled_status >> 32);

	ctrl_pipe_push_request(pipe, cmd, 3);
}

void ctrl_pipe_request_invalid(ctrl_pipe_t *pipe)
{
	unsigned int cmd[1];

	pr_err("Ctrl pipe received invalid command\n");

	cmd[0] = VEEAMSNAP_CHARCMD_INVALID;

	ctrl_pipe_push_request(pipe, cmd, 1);
}
