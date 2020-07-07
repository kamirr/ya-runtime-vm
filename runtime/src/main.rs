mod guest_agent_comm;
mod response_parser;

use std::io::{self, prelude::*};
use std::process::{Command, Stdio};

use crate::guest_agent_comm::{GuestAgent, Notification, RedirectFdType};

fn handle_notification(notification: Notification) {
    match notification {
        Notification::OutputAvailable { id, fd } => {
            println!("Process {} has output available on fd {}", id, fd);
        }
        Notification::ProcessDied { id, reason } => {
            println!("Process {} died with {:?}", id, reason);
        }
    }
}

fn run_process_with_output<F>(ga: &mut GuestAgent<F>, bin: &str, argv: &[&str]) -> io::Result<()>
where
    F: FnMut(Notification) -> (),
{
    let id = ga
        .run_process(bin, argv, None, 0, 0, &[None, None, None], None)?
        .expect("Run process failed");
    println!("Spawned process with id: {}", id);
    /* Wait for process to exit. */
    handle_notification(ga.get_one_notification()?);
    let out = ga
        .query_output(id, 0, u64::MAX)?
        .expect("Output query failed");
    println!("Output:");
    io::stdout().write_all(&out)?;
    Ok(())
}

fn main() -> io::Result<()> {
    let mut child = Command::new("qemu-system-x86_64")
        .args(&[
            "-m", "256m",
            "-nographic",
            "-vga", "none",
            "-kernel", "init-container/vmlinuz-virt",
            "-initrd", "init-container/initramfs.cpio.gz",
            "-no-reboot",
            "-net", "none",
            "-smp", "1",
            "-append", "console=ttyS0 panic=1",
            "-device", "virtio-serial",
            "-chardev", "socket,path=./manager.sock,server,nowait,id=manager_cdev",
            "-device", "virtserialport,chardev=manager_cdev,name=manager_port",
            "-drive", "file=./squashfs_drive,cache=none,readonly=on,format=raw,if=virtio"])
        .stdin(Stdio::null())
        .spawn()
        .expect("failed to spawn VM");

    let mut ga = GuestAgent::connected("./manager.sock", 10, handle_notification)?;

    let no_redir = [None, None, None];

    let id = ga
        .run_process(
            "/bin/ls",
            &["ls", "-al", "."],
            None,
            0,
            0,
            &no_redir,
            Some("/etc"),
        )?
        .expect("Run process failed");
    println!("Spawned process with id: {}", id);
    handle_notification(ga.get_one_notification()?);
    let out = ga
        .query_output(id, 0, u64::MAX)?
        .expect("Output query failed");
    println!("Output:");
    io::stdout().write_all(&out)?;

    run_process_with_output(&mut ga, "/bin/ls", &["ls", "-al", "/"])?;

    let fds = [
        None,
        Some(RedirectFdType::RedirectFdFile("/a".as_bytes())),
        None,
    ];
    let id = ga
        .run_process(
            "/bin/echo",
            &["echo", "TEST TEST TEST"],
            None,
            0,
            0,
            &fds,
            None,
        )?
        .expect("Run process failed");
    println!("Spawned process with id: {}", id);
    handle_notification(ga.get_one_notification()?);
    let out = ga
        .query_output(id, 0, u64::MAX)?
        .expect("Output query failed");
    println!("Output:");
    io::stdout().write_all(&out)?;

    run_process_with_output(&mut ga, "/bin/ls", &["ls", "-al", "/"])?;

    run_process_with_output(&mut ga, "/bin/cat", &["cat", "/a"])?;

    let id = ga
        .run_process("/bin/sleep", &["sleep", "10"], None, 0, 0, &no_redir, None)?
        .expect("Run process failed");
    println!("Spawned process with id: {}", id);

    ga.kill(id)?.expect("Kill failed");
    handle_notification(ga.get_one_notification()?);

    // ga.quit()?.expect("Quit failed");

    let id = ga
        .run_entrypoint("/bin/sleep", &["sleep", "2"], None, 0, 0, &no_redir, None)?
        .expect("Run process failed");
    println!("Spawned process with id: {}", id);
    /* Wait for entrypoint dying. */
    handle_notification(ga.get_one_notification()?);

    /* VM should quit now. */
    let e = child.wait().expect("failed to wait on child");
    println!("{:?}", e);

    Ok(())
}